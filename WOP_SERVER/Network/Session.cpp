#include "Session.h"
#include "EchoServer.h"
#include "RioApi.h"
#include "packet.h"
#include <cstdio>
#include <cstring>
#include <string>

/*-------------------
 클라이언트 액션 로그 문자열 변환
-------------------*/
namespace
{
    // Human-readable description of a client action, for the server console.
    // Falls back to the raw payload name for anything not called out below.
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
    /*-------------------
     생성/소멸
    -------------------*/
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

    /*-------------------
     RIO 송수신 큐잉
    -------------------*/
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

    /*-------------------
     멀티플레이어 브로드캐스트 처리 (로그인/이동/공격/아이템 사용)
    -------------------*/
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

                    // Also tell them what this player currently has equipped.
                    if (const uint8_t otherWeaponType = other->GetWeaponType(); otherWeaponType != 0)
                    {
                        flatbuffers::FlatBufferBuilder equipFbb;
                        auto equip = CreateS2C_ItemUseBroadcast(equipFbb, other->GetId(), ItemUseType::Equip, otherWeaponType);
                        auto equipReply = CreatePacket(equipFbb, Payload::S2C_ItemUseBroadcast, equip.Union());
                        FinishSizePrefixedPacketBuffer(equipFbb, equipReply);
                        EnqueueEcho(reinterpret_cast<const char*>(equipFbb.GetBufferPointer()),
                                    static_cast<uint32_t>(equipFbb.GetSize()));
                    }
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

            case Payload::C2S_AttackRequest:
            {
                const auto* req = packet->payload_as_C2S_AttackRequest();
                if (!req)
                    break;

                flatbuffers::FlatBufferBuilder fbb;
                const Vec3 origin = req->origin() ? *req->origin() : Vec3(0.0f, 0.0f, 0.0f);
                const Vec3 direction = req->direction() ? *req->direction() : Vec3(0.0f, 0.0f, 0.0f);
                auto broadcast = CreateS2C_AttackBroadcast(fbb, id_, req->weapon_slot(), req->attack_type(), &origin, &direction);
                auto reply = CreatePacket(fbb, Payload::S2C_AttackBroadcast, broadcast.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_ItemUseRequest:
            {
                const auto* req = packet->payload_as_C2S_ItemUseRequest();
                if (!req)
                    break;

                // Remember it for the roster loop above (new joiners).
                if (req->use_type() == ItemUseType::Equip)
                    weaponType_ = req->slot();

                // Broadcast every use_type; the client decides what to do.
                flatbuffers::FlatBufferBuilder fbb;
                auto broadcast = CreateS2C_ItemUseBroadcast(fbb, id_, req->use_type(), req->slot());
                auto reply = CreatePacket(fbb, Payload::S2C_ItemUseBroadcast, broadcast.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            default:
                break;
        }
    }

    /*-------------------
     수신 버퍼 프레이밍 (패킷 단위로 잘라서 처리)
    -------------------*/
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

            // Login/MoveInput already got a proper reply above; skip the
            // redundant self-echo for those two.
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

    /*-------------------
     RIO 완료 콜백 (워커 스레드에서 호출)
    -------------------*/
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

    /*-------------------
     세션 종료
    -------------------*/
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
