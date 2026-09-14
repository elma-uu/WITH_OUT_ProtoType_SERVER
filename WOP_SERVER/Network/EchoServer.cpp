#include "EchoServer.h"
#include "RioApi.h"
#include "packet.h"
#include <chrono>
#include <iterator>

namespace Wop
{
    /*-------------------
     생성/소멸
    -------------------*/
    EchoServer::EchoServer(uint16_t port, uint32_t workerThreadCount, uint32_t maxPlayers,
                           std::chrono::milliseconds matchWindow)
        : port_(port)
        , workerThreadCount_(workerThreadCount == 0 ? 1 : workerThreadCount)
        , maxPlayers_(maxPlayers == 0 ? 1 : maxPlayers)
        , matchmaker_(kMaxSquadSize, matchWindow,
              [this](std::vector<std::shared_ptr<Session>> squad) { CreateRoomForSquad(std::move(squad)); })
    {
    }

    EchoServer::~EchoServer()
    {
        Stop();
    }

    /*-------------------
     초기화 (Winsock/리슨소켓/AcceptEx/RIO)
    -------------------*/
    bool EchoServer::InitWinsock()
    {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            return false;
        winsockReady_ = true;
        return true;
    }

    bool EchoServer::CreateListenSocket()
    {
        listenSocket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                   WSA_FLAG_REGISTERED_IO | WSA_FLAG_OVERLAPPED);
        if (listenSocket_ == INVALID_SOCKET)
            return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
            return false;

        if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR)
            return false;

        return true;
    }

    bool EchoServer::LoadAcceptEx()
    {
        GUID guid = WSAID_ACCEPTEX;
        DWORD bytes = 0;
        const int result = WSAIoctl(
            listenSocket_, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guid, sizeof(guid),
            &acceptEx_, sizeof(acceptEx_),
            &bytes, nullptr, nullptr);
        return result != SOCKET_ERROR;
    }

    bool EchoServer::InitRio()
    {
        if (!RioApi::Get().Load(listenSocket_))
            return false;

        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, workerThreadCount_);
        if (iocp_ == nullptr)
            return false;

        const auto& rio = RioApi::Get().Table();

        RIO_NOTIFICATION_COMPLETION recvNotify{};
        recvNotify.Type = RIO_IOCP_COMPLETION;
        recvNotify.Iocp.IocpHandle = iocp_;
        recvNotify.Iocp.CompletionKey = reinterpret_cast<PVOID>(kRecvCompletionKey);
        recvNotify.Iocp.Overlapped = &recvNotifyOverlapped_;

        RIO_NOTIFICATION_COMPLETION sendNotify{};
        sendNotify.Type = RIO_IOCP_COMPLETION;
        sendNotify.Iocp.IocpHandle = iocp_;
        sendNotify.Iocp.CompletionKey = reinterpret_cast<PVOID>(kSendCompletionKey);
        sendNotify.Iocp.Overlapped = &sendNotifyOverlapped_;

        recvCq_ = rio.RIOCreateCompletionQueue(kCompletionQueueSize, &recvNotify);
        if (recvCq_ == RIO_INVALID_CQ)
            return false;

        sendCq_ = rio.RIOCreateCompletionQueue(kCompletionQueueSize, &sendNotify);
        if (sendCq_ == RIO_INVALID_CQ)
            return false;

        // Arm both queues; RIO notification is edge-triggered, so each queue
        // must be re-armed (see WorkerLoop) after every drain.
        rio.RIONotify(recvCq_);
        rio.RIONotify(sendCq_);
        return true;
    }

    /*-------------------
     시작/정지
    -------------------*/
    bool EchoServer::Start()
    {
        if (!InitWinsock())
        {
            std::printf("EchoServer: WSAStartup failed\n");
            return false;
        }
        if (!CreateListenSocket())
        {
            std::printf("EchoServer: failed to create listen socket (%d)\n", WSAGetLastError());
            return false;
        }
        if (!LoadAcceptEx())
        {
            std::printf("EchoServer: failed to load AcceptEx (%d)\n", WSAGetLastError());
            return false;
        }
        if (!InitRio())
        {
            std::printf("EchoServer: failed to initialize RIO (%d)\n", WSAGetLastError());
            return false;
        }

        running_ = true;

        workerThreads_.reserve(workerThreadCount_);
        for (uint32_t i = 0; i < workerThreadCount_; ++i)
            workerThreads_.emplace_back([this] { WorkerLoop(); });

        acceptThread_ = std::thread([this] { AcceptLoop(); });
        backgroundThread_ = std::thread([this] { BackgroundTickLoop(); });

        std::printf("EchoServer listening on port %u with %u worker thread(s)\n",
                     port_, workerThreadCount_);
        return true;
    }

    void EchoServer::Stop()
    {
        if (!running_.exchange(false))
            return;

        // Unblocks AcceptLoop: any AcceptEx currently waiting on the closed
        // listen socket completes with an error.
        if (listenSocket_ != INVALID_SOCKET)
        {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }

        if (acceptThread_.joinable())
            acceptThread_.join();

        if (backgroundThread_.joinable())
            backgroundThread_.join();

        for (size_t i = 0; i < workerThreads_.size(); ++i)
            PostQueuedCompletionStatus(iocp_, 0, kShutdownCompletionKey, nullptr);

        for (auto& t : workerThreads_)
            if (t.joinable())
                t.join();
        workerThreads_.clear();

        {
            std::lock_guard<std::mutex> guard(sessionsLock_);
            sessions_.clear();
        }

        const auto& rio = RioApi::Get().Table();
        if (recvCq_ != RIO_INVALID_CQ)
        {
            rio.RIOCloseCompletionQueue(recvCq_);
            recvCq_ = RIO_INVALID_CQ;
        }
        if (sendCq_ != RIO_INVALID_CQ)
        {
            rio.RIOCloseCompletionQueue(sendCq_);
            sendCq_ = RIO_INVALID_CQ;
        }

        if (iocp_ != nullptr)
        {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }

        if (winsockReady_)
        {
            WSACleanup();
            winsockReady_ = false;
        }
    }

    /*-------------------
     접속 수락 루프 / 세션 등록·해제
    -------------------*/
    void EchoServer::AcceptLoop()
    {
        while (running_.load(std::memory_order_acquire))
        {
            const SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                                   WSA_FLAG_REGISTERED_IO | WSA_FLAG_OVERLAPPED);
            if (clientSocket == INVALID_SOCKET)
            {
                if (running_.load(std::memory_order_acquire))
                    std::printf("AcceptLoop: WSASocket failed (%d)\n", WSAGetLastError());
                continue;
            }

            constexpr DWORD addrLen = sizeof(sockaddr_in) + 16;
            char addrBuf[addrLen * 2];
            DWORD bytesReceived = 0;

            OVERLAPPED ov{};
            ov.hEvent = WSACreateEvent();

            BOOL ok = acceptEx_(listenSocket_, clientSocket, addrBuf, 0,
                                 addrLen, addrLen, &bytesReceived, &ov);
            if (!ok)
            {
                if (WSAGetLastError() == ERROR_IO_PENDING &&
                    WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
                {
                    DWORD flags = 0;
                    ok = WSAGetOverlappedResult(clientSocket, &ov, &bytesReceived, FALSE, &flags);
                }
            }

            WSACloseEvent(ov.hEvent);

            if (!running_.load(std::memory_order_acquire))
            {
                closesocket(clientSocket);
                break;
            }

            if (!ok)
            {
                std::printf("AcceptLoop: AcceptEx failed (%d)\n", WSAGetLastError());
                closesocket(clientSocket);
                continue;
            }

            setsockopt(clientSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&listenSocket_), sizeof(listenSocket_));

            OnAccepted(clientSocket);
        }
    }

    void EchoServer::OnAccepted(SOCKET clientSocket)
    {
        {
            std::lock_guard<std::mutex> guard(sessionsLock_);
            if (sessions_.size() >= maxPlayers_)
            {
                std::printf("Rejecting connection: server full (%zu/%u players)\n", sessions_.size(), maxPlayers_);
                RejectServerFull(clientSocket);
                return;
            }
        }

        const uint32_t id = nextSessionId_.fetch_add(1, std::memory_order_relaxed);

        auto session = std::make_shared<Session>(
            clientSocket, id, recvCq_, sendCq_, *this,
            [this](uint32_t sessionId) { UnregisterSession(sessionId); });

        {
            std::lock_guard<std::mutex> guard(sessionsLock_);
            sessions_.emplace(id, session);
        }

        // No Room yet -- this session doesn't get one until it actually
        // logs in and is matched (see EnqueueForMatch, called from
        // Session's own C2S_Login handling).
        if (!session->Start())
        {
            std::printf("[Session %u] failed to initialize RIO\n", id);
            UnregisterSession(id);
            return;
        }

        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        if (getpeername(clientSocket, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0)
        {
            char ipStr[INET_ADDRSTRLEN] = {};
            InetNtopA(AF_INET, &peer.sin_addr, ipStr, sizeof(ipStr));
            std::printf("[Session %u] connected from %s:%u\n", id, ipStr, ntohs(peer.sin_port));
        }
    }

    void EchoServer::RejectServerFull(SOCKET clientSocket)
    {
        // No S2C_LoginFail here, deliberately: this socket was created with
        // WSA_FLAG_REGISTERED_IO (see AcceptLoop/CreateListenSocket), and
        // MSDN is explicit that such a socket only supports the RIO
        // function table -- plain send() fails on it with WSAENOTSOCK
        // (confirmed while building this). Routing the rejection through a
        // real Session just to say "full" would mean either registering it
        // in sessions_ (defeating the cap until the client disconnects
        // itself) or closesocket()'ing right after posting a RIOSend, which
        // races the in-flight completion. Simplest correct option: just
        // close: the client's own "unexpected disconnect" handling (see
        // UProtoNetClientSubsystem::Tick) already re-shows the connect
        // prompt.
        closesocket(clientSocket);
    }

    void EchoServer::UnregisterSession(uint32_t sessionId)
    {
        std::shared_ptr<Session> keepAlive;
        {
            std::lock_guard<std::mutex> guard(sessionsLock_);
            auto it = sessions_.find(sessionId);
            if (it != sessions_.end())
            {
                keepAlive = std::move(it->second);
                sessions_.erase(it);
            }
        }

        if (keepAlive)
        {
            // A session disconnecting before it was ever matched (still
            // sitting in matchmaker_'s queue -- see EnqueueForMatch) would
            // otherwise linger there forever, or get handed to
            // CreateRoomForSquad as a stale entry once a squad forms
            // around it. No-op if it already matched (and so isn't queued
            // anymore) or never queued to begin with.
            matchmaker_.Cancel(sessionId);

            // A disconnecting session was, by definition, still a member of
            // its Room an instant ago (if it ever got one) -- remove it
            // first so the "is anyone still visible" check below (and
            // every Broadcast after it) doesn't count the session that's
            // leaving.
            const std::shared_ptr<Room> room = keepAlive->GetRoom();
            if (room)
            {
                room->RemoveSession(sessionId);

                // If this was the last VISIBLE member of that room (or it's
                // simply empty now), reset ITS world state -- see Room::
                // ResetWorldStateIfNoVisibleMembers's comment for why
                // "visible", not "connected/a member", is the right signal.
                room->ResetWorldStateIfNoVisibleMembers();

                // And if it's now completely empty (not just invisible),
                // there's no reason to keep it around at all -- unlike step
                // 1's single server-lifetime defaultRoom_, real matchmaking
                // creates rooms on demand, so they should go away on demand
                // too, or a long-running server would just accumulate one
                // forever-empty Room per match ever played.
                if (room->MemberCount() == 0)
                {
                    std::lock_guard<std::mutex> guard(roomsLock_);
                    rooms_.erase(room->GetId());
                }
            }

            std::printf("[Session %u] disconnected\n", sessionId);

            // Best-effort final save so a reconnect picks up close to where
            // this session left off (no-op if it never logged into an
            // account -- see Session::FlushProgress).
            keepAlive->FlushProgress();

            if (room)
            {
                // Tell the rest of that room so they can despawn this
                // player's remote actor instead of leaving a frozen ghost.
                flatbuffers::FlatBufferBuilder fbb;
                auto left = ProtoType::Net::CreateS2C_PlayerLeft(fbb, sessionId);
                auto packet = ProtoType::Net::CreatePacket(fbb, ProtoType::Net::Payload::S2C_PlayerLeft, left.Union());
                ProtoType::Net::FinishSizePrefixedPacketBuffer(fbb, packet);
                room->Broadcast(sessionId, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                          static_cast<uint32_t>(fbb.GetSize()));

                // Any enemy this session was driving needs a new owner -- it
                // doesn't despawn just because its driver left.
                for (uint32_t enemyId : room->ReleaseEnemiesOwnedBy(sessionId))
                {
                    flatbuffers::FlatBufferBuilder ownerLeftFbb;
                    auto ownerLeft = ProtoType::Net::CreateS2C_EnemyOwnerLeft(ownerLeftFbb, enemyId);
                    auto ownerLeftPacket = ProtoType::Net::CreatePacket(ownerLeftFbb, ProtoType::Net::Payload::S2C_EnemyOwnerLeft, ownerLeft.Union());
                    ProtoType::Net::FinishSizePrefixedPacketBuffer(ownerLeftFbb, ownerLeftPacket);
                    room->Broadcast(sessionId, reinterpret_cast<const char*>(ownerLeftFbb.GetBufferPointer()),
                              static_cast<uint32_t>(ownerLeftFbb.GetSize()));
                }
            }
        }

        // `keepAlive` drops here, destroying the Session (and deregistering
        // its RIO buffers) outside of sessionsLock_.
    }

    /*-------------------
     매치메이킹 / Room 생성
    -------------------*/
    void EchoServer::LoadEnemyObstacles(const std::string& path)
    {
        // Stored, not applied to anything yet -- there's no Room to apply
        // it to until a squad actually forms (see CreateRoomForSquad).
        // Matches main.cpp's existing call site (before Start()), so no
        // Room exists yet regardless.
        enemyObstaclesPath_ = path;
    }

    void EchoServer::EnqueueForMatch(std::shared_ptr<Session> session)
    {
        if (!session)
            return;

        // Prefer joining an existing room with room to spare over starting
        // a fresh squad -- lets a friend join an in-progress raid instead
        // of only ever forming brand new ones (see door_late_join_test's
        // whole scenario: whoever's already mid-raid shouldn't become
        // unreachable to a session that logs in a moment later). Room::
        // AddSession fires Room::AnnounceNewMember, which gives this
        // session the exact same roster/door-state replay a step-1-style
        // single shared world always gave a late joiner.
        {
            std::lock_guard<std::mutex> guard(roomsLock_);
            for (const auto& [roomId, room] : rooms_)
            {
                if (room->MemberCount() < kMaxSquadSize)
                {
                    // 진단 로그(문제: "먼저 들어온 사람 화면에서 늦게 들어온
                    // 유저가 안 보임") -- 이 세션이 실제로 "이미 자리 있는
                    // 기존 방"으로 late-join하고 있는지, 그 방에 지금 몇 명이
                    // 있는지 서버 콘솔에서 바로 확인하기 위함.
                    std::printf("[Room %u] EnqueueForMatch: session %u late-joining (room had %zu member(s) already)\n",
                                roomId, session->GetId(), room->MemberCount());

                    // This session never goes through matchmaker_, so it
                    // would otherwise never get ANY S2C_MatchmakingComplete
                    // -- the client (LevelChangeSelectWidget) is waiting on
                    // exactly that one signal regardless of which path
                    // produced it (see Matchmaker::FormSquadLocked's own
                    // send of the same message). Sent before AddSession
                    // moves `session` out from under us, and before
                    // AddSession's own roster/door-replay sends, matching
                    // the ordering Matchmaker's path already guarantees.
                    {
                        using namespace ProtoType::Net;
                        flatbuffers::FlatBufferBuilder fbb;
                        auto complete = CreateS2C_MatchmakingComplete(
                            fbb, static_cast<uint16_t>(room->MemberCount() + 1));
                        auto packet = CreatePacket(fbb, Payload::S2C_MatchmakingComplete, complete.Union());
                        FinishSizePrefixedPacketBuffer(fbb, packet);
                        session->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                      static_cast<uint32_t>(fbb.GetSize()));
                    }
                    room->AddSession(std::move(session));
                    return;
                }
            }
        }

        // No room has space -- queue for a fresh squad instead (see
        // Matchmaker.h: forms immediately once the queue fills up, or after
        // matchWindow_ elapses if it never does).
        matchmaker_.Enqueue(std::move(session));
    }

    void EchoServer::CreateRoomForSquad(std::vector<std::shared_ptr<Session>> squad)
    {
        if (squad.empty())
            return;

        const uint32_t roomId = nextRoomId_.fetch_add(1, std::memory_order_relaxed);
        auto room = std::make_shared<Room>(roomId);
        if (!enemyObstaclesPath_.empty())
            room->LoadEnemyObstacles(enemyObstaclesPath_);

        {
            std::lock_guard<std::mutex> guard(roomsLock_);
            rooms_.emplace(roomId, room);
        }

        std::printf("[Room %u] formed with %zu player(s)\n", roomId, squad.size());
        for (auto& session : squad)
            room->AddSession(std::move(session));
    }

    void EchoServer::BackgroundTickLoop()
    {
        // 100ms(기존 150ms에서 단축) -- 좀비 공격 판정이 쓰는 플레이어 위치가
        // 클라이언트의 NetSyncInterval(100ms)만큼만 뒤처지게 하기 위함. 150ms
        // 그대로였으면 100(클라 전송 주기)+150(이 틱 주기) = 최대 250ms까지
        // 벌어질 수 있었고, 좀비 기본 공격 사거리(150 유닛 안팎)와 비슷한
        // 크기라 빠르게 움직이는 플레이어를 "허공에 대고 공격"하는 것처럼
        // 보이는 원인이 됐다. Matchmaker::TickTimeouts()도 같은 루프에서 이
        // 간격으로 도는데, 최대 대기 시간(10초) 자체는 그대로고 그냥 더
        // 촘촘히 체크할 뿐이라 무해함.
        constexpr auto kTickInterval = std::chrono::milliseconds(100);
        auto lastTick = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kTickInterval);
            if (!running_.load(std::memory_order_acquire))
                break;

            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;

            // Forms an under-sized (down to solo) squad for whoever's been
            // waiting alone too long -- see Matchmaker::TickTimeouts.
            matchmaker_.TickTimeouts();

            // Snapshot the room list before ticking: CreateRoomForSquad or
            // UnregisterSession's empty-room cleanup could otherwise race
            // this loop iterating rooms_ directly.
            std::vector<std::shared_ptr<Room>> roomsSnapshot;
            {
                std::lock_guard<std::mutex> guard(roomsLock_);
                roomsSnapshot.reserve(rooms_.size());
                for (const auto& [roomId, room] : rooms_)
                    roomsSnapshot.push_back(room);
            }

            for (const auto& room : roomsSnapshot)
                room->Tick(deltaSeconds);
        }
    }

    /*-------------------
     워커 스레드 루프 (IOCP 대기 → RIO 완료 드레인)
    -------------------*/
    void EchoServer::WorkerLoop()
    {
        RIORESULT results[64];

        for (;;)
        {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED overlapped = nullptr;

            const BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &overlapped, INFINITE);
            if (key == kShutdownCompletionKey)
                break;

            if (!ok && overlapped == nullptr)
                continue;

            const bool isRecvQueue = (key == kRecvCompletionKey);
            const bool isSendQueue = (key == kSendCompletionKey);
            if (!isRecvQueue && !isSendQueue)
                continue;

            const RIO_CQ cq = isRecvQueue ? recvCq_ : sendCq_;
            const auto& rio = RioApi::Get().Table();

            for (;;)
            {
                const ULONG count = rio.RIODequeueCompletion(
                    cq, results, static_cast<ULONG>(std::size(results)));
                if (count == 0 || count == RIO_CORRUPT_CQ)
                    break;

                for (ULONG i = 0; i < count; ++i)
                {
                    auto* session = reinterpret_cast<Session*>(results[i].SocketContext);
                    const bool success = (results[i].Status == 0);
                    const auto bytesTransferred = static_cast<uint32_t>(results[i].BytesTransferred);

                    if (isRecvQueue)
                        session->OnRecvCompletion(success, bytesTransferred);
                    else
                        session->OnSendCompletion(success, bytesTransferred);
                }

                if (count < std::size(results))
                    break;
            }

            rio.RIONotify(cq);
        }
    }
}
