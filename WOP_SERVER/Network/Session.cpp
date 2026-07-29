#include "Session.h"
#include "EchoServer.h"
#include "RioApi.h"
#include "packet.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // Human-readable description of a client action, for the server console.
    // Falls back to the raw payload name for anything not called out below
    // (S2C_* payloads should never arrive from a client in the first place).
    const char* DescribeAction(const ProtoType::Net::Packet* packet)
    {
        using namespace ProtoType::Net;

        switch (packet->payload_type())
        {
            case Payload::C2S_Login:
                return "login";

            case Payload::C2S_MoveInput:
                return "move";

            case Payload::C2S_AttackRequest:
                if (const auto* req = packet->payload_as_C2S_AttackRequest())
                {
                    switch (req->attack_type())
                    {
                        case AttackType::Fire:        return "attack fire";
                        case AttackType::Melee:       return "attack melee";
                        case AttackType::Throw:       return "attack throw";
                        case AttackType::ReloadStart: return "reload start";
                    }
                }
                return "attack";

            case Payload::C2S_ItemUseRequest:
                if (const auto* req = packet->payload_as_C2S_ItemUseRequest())
                {
                    switch (req->use_type())
                    {
                        case ItemUseType::Consume:          return "item consume";
                        case ItemUseType::Equip:            return "item equip";
                        case ItemUseType::Unequip:           return "item unequip";
                        case ItemUseType::Drop:             return "item drop";
                        case ItemUseType::Reload:            return "weapon reload";
                        case ItemUseType::Inspect:          return "item inspect";
                        case ItemUseType::ToggleAttachment: return "attachment toggle";
                    }
                }
                return "item use";

            case Payload::C2S_InteractRequest:
                if (const auto* req = packet->payload_as_C2S_InteractRequest())
                {
                    switch (req->interact_type())
                    {
                        // Both weapon and item pickups go through the same
                        // world-loot interaction; the protocol has no
                        // separate field to tell them apart.
                        case InteractType::Loot:       return "item get";
                        case InteractType::DoorOpen:   return "door open";
                        case InteractType::DoorClose:  return "door close";
                        case InteractType::Extract:    return "extract";
                        case InteractType::PlantItem:  return "item plant";
                        case InteractType::UseSwitch:  return "switch use";
                    }
                }
                return "interact";

            default:
                return EnumNamePayload(packet->payload_type());
        }
    }
}

namespace Wop
{
    Session::Session(SOCKET socket, uint32_t id, RIO_CQ recvCq, RIO_CQ sendCq,
                      EchoServer& server, std::function<void(uint32_t)> onClosed)
        : socket_(socket)
        , id_(id)
        , recvCq_(recvCq)
        , sendCq_(sendCq)
        , server_(server)
        , onClosed_(std::move(onClosed))
    {
    }

    Session::~Session()
    {
        if (socket_ != INVALID_SOCKET)
            closesocket(socket_);

        const auto& rio = RioApi::Get().Table();
        if (recvBufferId_ != RIO_INVALID_BUFFERID)
            rio.RIODeregisterBuffer(recvBufferId_);
        if (sendBufferId_ != RIO_INVALID_BUFFERID)
            rio.RIODeregisterBuffer(sendBufferId_);
    }

    bool Session::Start()
    {
        const auto& rio = RioApi::Get().Table();

        recvBufferId_ = rio.RIORegisterBuffer(recvBuffer_.Base(), recvBuffer_.Capacity());
        if (recvBufferId_ == RIO_INVALID_BUFFERID)
            return false;

        sendBufferId_ = rio.RIORegisterBuffer(sendBuffer_.Base(), sendBuffer_.Capacity());
        if (sendBufferId_ == RIO_INVALID_BUFFERID)
            return false;

        rq_ = rio.RIOCreateRequestQueue(
            socket_,
            /*MaxOutstandingReceive*/ 1, /*MaxReceiveDataBuffers*/ 1,
            /*MaxOutstandingSend*/ 1, /*MaxSendDataBuffers*/ 1,
            recvCq_, sendCq_, this);
        if (rq_ == RIO_INVALID_RQ)
            return false;

        std::lock_guard<std::mutex> guard(lock_);
        return PostRecv();
    }

    bool Session::PostRecv()
    {
        if (!recvBuffer_.ReserveWritable(1))
            return false;

        RIO_BUF buf{};
        buf.BufferId = recvBufferId_;
        buf.Offset = recvBuffer_.WriteOffset();
        buf.Length = recvBuffer_.TailFreeSize();

        pendingOps_.fetch_add(1, std::memory_order_acq_rel);
        if (!RioApi::Get().Table().RIOReceive(rq_, &buf, 1, 0, kRecvRequestTag))
        {
            pendingOps_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    void Session::TryPostSend()
    {
        if (sendInProgress_)
            return;

        const uint32_t size = sendBuffer_.DataSize();
        if (size == 0)
            return;

        RIO_BUF buf{};
        buf.BufferId = sendBufferId_;
        buf.Offset = sendBuffer_.ReadOffset();
        buf.Length = size;

        pendingOps_.fetch_add(1, std::memory_order_acq_rel);
        if (!RioApi::Get().Table().RIOSend(rq_, &buf, 1, 0, kSendRequestTag))
        {
            pendingOps_.fetch_sub(1, std::memory_order_acq_rel);
            Close("failed to post send");
            return;
        }
        sendInProgress_ = true;
    }

    void Session::EnqueueEcho(const char* data, uint32_t len)
    {
        if (!sendBuffer_.ReserveWritable(len))
        {
            Close("send buffer overflow");
            return;
        }

        std::memcpy(sendBuffer_.WritePos(), data, len);
        sendBuffer_.OnWrite(len);
        TryPostSend();
    }

    void Session::Send(const char* data, uint32_t len)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (closing_.load(std::memory_order_acquire))
            return;

        EnqueueEcho(data, len);
    }

    void Session::BroadcastGameplayState(const ProtoType::Net::Packet* packet)
    {
        using namespace ProtoType::Net;

        switch (packet->payload_type())
        {
            case Payload::C2S_Login:
            {
                // Cheap deterministic spawn point so players don't stack.
                position_ = Vec3(static_cast<float>(id_ % 8) * 200.0f, 0.0f, 100.0f);
                look_ = Rotator(0.0f, 0.0f, 0.0f);

                // 1) Tell this client its own player id.
                {
                    flatbuffers::FlatBufferBuilder fbb;
                    auto success = CreateS2C_LoginSuccess(fbb, id_);
                    auto reply = CreatePacket(fbb, Payload::S2C_LoginSuccess, success.Union());
                    FinishSizePrefixedPacketBuffer(fbb, reply);
                    EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                static_cast<uint32_t>(fbb.GetSize()));
                }

                // 2) Tell this client about everyone already connected.
                for (const auto& other : server_.SnapshotOtherSessions(id_))
                {
                    flatbuffers::FlatBufferBuilder fbb;
                    const std::string nickname = "Player" + std::to_string(other->GetId());
                    auto nicknameOffset = fbb.CreateString(nickname);
                    const Vec3 otherPos = other->GetPosition();
                    const Rotator otherLook = other->GetLook();
                    auto info = CreateS2C_SendPlayerInfo(fbb, other->GetId(), nicknameOffset, &otherPos, &otherLook, 0, 0);
                    auto reply = CreatePacket(fbb, Payload::S2C_SendPlayerInfo, info.Union());
                    FinishSizePrefixedPacketBuffer(fbb, reply);
                    EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                static_cast<uint32_t>(fbb.GetSize()));
                }

                // 3) Tell everyone else that this player just joined.
                {
                    flatbuffers::FlatBufferBuilder fbb;
                    const std::string nickname = "Player" + std::to_string(id_);
                    auto nicknameOffset = fbb.CreateString(nickname);
                    auto info = CreateS2C_SendPlayerInfo(fbb, id_, nicknameOffset, &position_, &look_, 0, 0);
                    auto reply = CreatePacket(fbb, Payload::S2C_SendPlayerInfo, info.Union());
                    FinishSizePrefixedPacketBuffer(fbb, reply);
                    server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                       static_cast<uint32_t>(fbb.GetSize()));
                }
                break;
            }

            case Payload::C2S_MoveInput:
            {
                const auto* move = packet->payload_as_C2S_MoveInput();
                if (!move)
                    break;

                if (const auto* pos = move->position())
                    position_ = *pos;
                if (const auto* lookField = move->look())
                    look_ = *lookField;

                flatbuffers::FlatBufferBuilder fbb;
                const Vec3 velocity(0.0f, 0.0f, 0.0f);
                const uint32_t ackSeq = move->header() ? move->header()->seq() : 0;
                auto state = CreateS2C_MoveState(fbb, id_, 0, &position_, &velocity, &look_, move->flags(), ackSeq);
                auto reply = CreatePacket(fbb, Payload::S2C_MoveState, state.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            default:
                break;
        }
    }

    void Session::ProcessRecvBuffer()
    {
        for (;;)
        {
            const uint32_t available = recvBuffer_.DataSize();
            if (available < sizeof(uint32_t))
                break;

            uint32_t bodySize = 0;
            std::memcpy(&bodySize, recvBuffer_.ReadPos(), sizeof(bodySize));

            const uint64_t total = static_cast<uint64_t>(sizeof(bodySize)) + bodySize;
            if (total > recvBuffer_.Capacity())
            {
                Close("packet larger than the receive buffer");
                return;
            }

            if (available < total)
                break;

            flatbuffers::Verifier verifier(
                reinterpret_cast<const uint8_t*>(recvBuffer_.ReadPos()),
                static_cast<size_t>(total));

            if (!ProtoType::Net::VerifySizePrefixedPacketBuffer(verifier))
            {
                Close("invalid packet framing");
                return;
            }

            const auto* packet = ProtoType::Net::GetSizePrefixedPacket(recvBuffer_.ReadPos());
            std::printf("[ Client %u ] %s\n", id_, DescribeAction(packet));

            BroadcastGameplayState(packet);

            // Login/MoveInput already get a proper reply (S2C_LoginSuccess +
            // roster/join broadcast, or S2C_MoveState broadcast) above; also
            // self-echoing the raw C2S_* packet would just be stream noise
            // that could be mistaken for a real S2C_* message.
            const auto type = packet->payload_type();
            const bool skipSelfEcho =
                (type == ProtoType::Net::Payload::C2S_Login || type == ProtoType::Net::Payload::C2S_MoveInput);
            if (!skipSelfEcho)
                EnqueueEcho(recvBuffer_.ReadPos(), static_cast<uint32_t>(total));
            if (closing_.load(std::memory_order_acquire))
                return;

            recvBuffer_.OnRead(static_cast<uint32_t>(total));
        }
    }

    void Session::OnRecvCompletion(bool success, uint32_t bytesTransferred)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            pendingOps_.fetch_sub(1, std::memory_order_acq_rel);

            if (!success || bytesTransferred == 0)
            {
                Close(!success ? "recv failed" : "peer closed connection");
            }
            else
            {
                recvBuffer_.OnWrite(bytesTransferred);
                ProcessRecvBuffer();

                if (!closing_.load(std::memory_order_acquire))
                {
                    if (!PostRecv())
                        Close("failed to re-post recv");
                }
            }
        }

        ReleaseIfIdle();
    }

    void Session::OnSendCompletion(bool success, uint32_t bytesTransferred)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            pendingOps_.fetch_sub(1, std::memory_order_acq_rel);
            sendInProgress_ = false;

            if (!success)
            {
                Close("send failed");
            }
            else
            {
                sendBuffer_.OnRead(bytesTransferred);
                if (!closing_.load(std::memory_order_acquire))
                    TryPostSend();
            }
        }

        ReleaseIfIdle();
    }

    void Session::Close(const char* reason)
    {
        if (closing_.exchange(true, std::memory_order_acq_rel))
            return; // already closing

        std::printf("[Session %u] closing (%s)\n", id_, reason);
        closesocket(socket_);
    }

    void Session::ReleaseIfIdle()
    {
        if (!closing_.load(std::memory_order_acquire))
            return;

        if (pendingOps_.load(std::memory_order_acquire) != 0)
            return;

        if (released_.exchange(true, std::memory_order_acq_rel))
            return; // a racing completion already released this session

        if (onClosed_)
            onClosed_(id_);
    }
}
