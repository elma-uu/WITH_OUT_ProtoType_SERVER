#include "EchoServer.h"
#include "RioApi.h"
#include "packet.h"
#include <iterator>

namespace Wop
{
    /*-------------------
     생성/소멸
    -------------------*/
    EchoServer::EchoServer(uint16_t port, uint32_t workerThreadCount, uint32_t maxPlayers)
        : port_(port)
        , workerThreadCount_(workerThreadCount == 0 ? 1 : workerThreadCount)
        , maxPlayers_(maxPlayers == 0 ? 1 : maxPlayers)
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
            std::printf("[Session %u] disconnected\n", sessionId);

            // Best-effort final save so a reconnect picks up close to where
            // this session left off (no-op if it never logged into an
            // account -- see Session::FlushProgress).
            keepAlive->FlushProgress();

            // Tell everyone still connected so they can despawn this
            // player's remote actor instead of leaving a frozen ghost.
            flatbuffers::FlatBufferBuilder fbb;
            auto left = ProtoType::Net::CreateS2C_PlayerLeft(fbb, sessionId);
            auto packet = ProtoType::Net::CreatePacket(fbb, ProtoType::Net::Payload::S2C_PlayerLeft, left.Union());
            ProtoType::Net::FinishSizePrefixedPacketBuffer(fbb, packet);
            Broadcast(sessionId, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                      static_cast<uint32_t>(fbb.GetSize()));
        }

        // `keepAlive` drops here, destroying the Session (and deregistering
        // its RIO buffers) outside of sessionsLock_.
    }

    /*-------------------
     멀티플레이어 브로드캐스트 지원
    -------------------*/
    void EchoServer::Broadcast(uint32_t excludeSessionId, const char* data, uint32_t len)
    {
        for (const auto& session : SnapshotOtherSessions(excludeSessionId))
            session->Send(data, len);
    }

    std::vector<std::shared_ptr<Session>> EchoServer::SnapshotOtherSessions(uint32_t excludeSessionId)
    {
        std::vector<std::shared_ptr<Session>> result;
        std::lock_guard<std::mutex> guard(sessionsLock_);
        result.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_)
        {
            if (id != excludeSessionId)
                result.push_back(session);
        }
        return result;
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
