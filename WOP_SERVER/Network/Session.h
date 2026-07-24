#pragma once
#include "NetCommon.h"
#include "RingBuffer.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace Wop
{
    // One TCP connection. Owns two RIO-registered ring buffers (recv/send),
    // a Registered I/O request queue, and the framing logic that turns the
    // raw byte stream into complete size-prefixed Packet buffers which are
    // then echoed back verbatim.
    class Session : public std::enable_shared_from_this<Session>
    {
    public:
        static constexpr uint32_t kRecvBufferCapacity = 64 * 1024;
        static constexpr uint32_t kSendBufferCapacity = 256 * 1024;

        Session(SOCKET socket, uint32_t id, RIO_CQ recvCq, RIO_CQ sendCq,
                std::function<void(uint32_t)> onClosed);
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Registers the RIO buffers, creates the request queue and posts the
        // first receive. Returns false if any RIO setup call fails.
        bool Start();

        // Invoked by the worker threads once RIODequeueCompletion hands back
        // a result whose RequestContext identifies it as a recv/send.
        void OnRecvCompletion(bool success, uint32_t bytesTransferred);
        void OnSendCompletion(bool success, uint32_t bytesTransferred);

        SOCKET GetSocket() const { return socket_; }
        uint32_t GetId() const { return id_; }

    private:
        bool PostRecv();      // caller must hold lock_
        void TryPostSend();   // caller must hold lock_
        void ProcessRecvBuffer(); // caller must hold lock_
        void EnqueueEcho(const char* data, uint32_t len); // caller must hold lock_

        void Close(const char* reason);
        void ReleaseIfIdle(); // erases the session once no RIO op is in flight

        SOCKET socket_;
        uint32_t id_;
        RIO_CQ recvCq_;
        RIO_CQ sendCq_;
        std::function<void(uint32_t)> onClosed_;

        RIO_RQ rq_ = RIO_INVALID_RQ;
        RIO_BUFFERID recvBufferId_ = RIO_INVALID_BUFFERID;
        RIO_BUFFERID sendBufferId_ = RIO_INVALID_BUFFERID;

        RingBuffer recvBuffer_{kRecvBufferCapacity};
        RingBuffer sendBuffer_{kSendBufferCapacity};

        std::mutex lock_;
        bool sendInProgress_ = false;

        std::atomic<int> pendingOps_{0};
        std::atomic<bool> closing_{false};
        std::atomic<bool> released_{false};
    };
}
