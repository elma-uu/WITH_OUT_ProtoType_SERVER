#pragma once
#include "NetCommon.h"
#include "Session.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Wop
{
    class EchoServer
    {
    public:
        /*-------------------
         생성/시작/정지
        -------------------*/
        EchoServer(uint16_t port, uint32_t workerThreadCount);
        ~EchoServer();

        EchoServer(const EchoServer&) = delete;
        EchoServer& operator=(const EchoServer&) = delete;

        bool Start();
        void Stop();

        /*-------------------
         멀티플레이어 브로드캐스트 지원
        -------------------*/
        // Sends `data` to every connected session except `excludeSessionId`.
        void Broadcast(uint32_t excludeSessionId, const char* data, uint32_t len);

        // Snapshot of every currently-connected session other than
        // `excludeSessionId`, for building a "who's already here" roster.
        std::vector<std::shared_ptr<Session>> SnapshotOtherSessions(uint32_t excludeSessionId);

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

        void OnAccepted(SOCKET clientSocket);
        void UnregisterSession(uint32_t sessionId);

        /*-------------------
         멤버 변수
        -------------------*/
        uint16_t port_;
        uint32_t workerThreadCount_;

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

        std::mutex sessionsLock_;
        std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
        std::atomic<uint32_t> nextSessionId_{1};
    };
}
