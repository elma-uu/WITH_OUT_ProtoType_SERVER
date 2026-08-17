#include "Session.h"
#include "EchoServer.h"
#include "RioApi.h"
#include "Database.h"
#include "packet.h"
#include <cmath>
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

            case Payload::C2S_SaveInventory:
                return "save inventory";

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
                const auto* req = packet->payload_as_C2S_Login();

                // Real account auth is opt-in: only attempted if the client
                // actually sent credentials (older/test clients that only
                // send auth_token still log in with no persistence, same as
                // before this feature existed).
                bool hasSavedProgress = false;
                Vec3 savedPosition{};
                Rotator savedLook{};
                uint8_t savedWeaponType = 0;
                std::vector<InventoryItemRecord> savedInventory;

                if (req && req->username() && req->username()->size() > 0
                    && req->password() && Database::Get().IsConnected())
                {
                    const std::string username = req->username()->str();
                    const std::string password = req->password()->str();
                    const bool isRegister = req->is_register();

                    int accountId = -1;
                    const AuthResult result = isRegister
                        ? Database::Get().Register(username, password, accountId)
                        : Database::Get().Authenticate(username, password, accountId);

                    if (result != AuthResult::Success)
                    {
                        // Reject outright, don't spawn this session into the game.
                        LoginFailReason reason = LoginFailReason::Unknown;
                        const char* message = "Unknown error.";
                        switch (result)
                        {
                            case AuthResult::AccountNotFound:
                                reason = LoginFailReason::AccountNotFound;
                                message = "No account with that username.";
                                break;
                            case AuthResult::WrongPassword:
                                reason = LoginFailReason::InvalidToken;
                                message = "Invalid username or password.";
                                break;
                            case AuthResult::UsernameTaken:
                                reason = LoginFailReason::UsernameTaken;
                                message = "That username is already taken.";
                                break;
                            default:
                                reason = LoginFailReason::Unknown;
                                message = "Login failed (server error).";
                                break;
                        }

                        flatbuffers::FlatBufferBuilder fbb;
                        auto messageOffset = fbb.CreateString(message);
                        auto fail = CreateS2C_LoginFail(fbb, reason, messageOffset);
                        auto reply = CreatePacket(fbb, Payload::S2C_LoginFail, fail.Union());
                        FinishSizePrefixedPacketBuffer(fbb, reply);
                        EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                    static_cast<uint32_t>(fbb.GetSize()));
                        break;
                    }

                    accountId_ = accountId;
                    hasSavedProgress = Database::Get().LoadProgress(accountId, savedPosition, savedLook, savedWeaponType);
                    Database::Get().LoadInventory(accountId, savedInventory);
                }

                if (hasSavedProgress)
                {
                    position_ = savedPosition;
                    look_ = savedLook;
                    weaponType_ = savedWeaponType;
                }
                else
                {
                    // Cheap deterministic spawn point so players don't stack.
                    // Grid instead of a single row now that maxPlayers can be
                    // well above 8 (see main.cpp) -- a plain "id_ % 8" would
                    // start reusing X positions past the 9th player.
                    position_ = Vec3(static_cast<float>(id_ % 8) * 200.0f, static_cast<float>((id_ / 8) % 8) * 200.0f, 100.0f);
                    look_ = Rotator(0.0f, 0.0f, 0.0f);
                }

                // 1) Tell this client its own player id (+ restored
                //    position/weapon/inventory, if this account had saved progress).
                {
                    flatbuffers::FlatBufferBuilder fbb;
                    std::vector<flatbuffers::Offset<InventoryItemEntry>> inventoryOffsets;
                    inventoryOffsets.reserve(savedInventory.size());
                    for (const auto& item : savedInventory)
                    {
                        auto itemIdOffset = fbb.CreateString(item.itemId);
                        inventoryOffsets.push_back(CreateInventoryItemEntry(
                            fbb, itemIdOffset, item.gridX, item.gridY, item.rotated, item.stackCount));
                    }
                    auto inventoryVector = fbb.CreateVector(inventoryOffsets);

                    auto success = CreateS2C_LoginSuccess(fbb, id_, &position_, &look_, weaponType_, hasSavedProgress, inventoryVector);
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

                    // Also tell the newly-joining client what this existing
                    // player is currently holding, so their weapon shows up
                    // right away instead of only on their next weapon swap.
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

                // If restored progress means this player already has a
                // weapon out, tell everyone else too (mirrors step 2 above,
                // just in the other direction).
                if (weaponType_ != 0)
                {
                    flatbuffers::FlatBufferBuilder equipFbb;
                    auto equip = CreateS2C_ItemUseBroadcast(equipFbb, id_, ItemUseType::Equip, weaponType_);
                    auto equipReply = CreatePacket(equipFbb, Payload::S2C_ItemUseBroadcast, equip.Union());
                    FinishSizePrefixedPacketBuffer(equipFbb, equipReply);
                    server_.Broadcast(id_, reinterpret_cast<const char*>(equipFbb.GetBufferPointer()),
                                       static_cast<uint32_t>(equipFbb.GetSize()));
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

                // Throttled progress save: MoveInput arrives ~10/sec while a
                // player is active, far too often for a DB write, so only
                // persist every few seconds. A final save also happens on
                // disconnect (FlushProgress), so this is just "don't lose
                // more than a few seconds" rather than a strict guarantee.
                if (accountId_ >= 0)
                {
                    constexpr std::chrono::seconds kSaveInterval(5);
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastProgressSave_ >= kSaveInterval)
                    {
                        lastProgressSave_ = now;
                        Database::Get().SaveProgress(accountId_, position_, look_, weaponType_);
                    }
                }

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

                ResolveAndBroadcastHit(origin, direction, req->weapon_slot());
                break;
            }

            case Payload::C2S_ItemUseRequest:
            {
                const auto* req = packet->payload_as_C2S_ItemUseRequest();
                if (!req)
                    break;

                // Remember what this session is currently holding so future
                // joiners can be told about it immediately (see the roster
                // loop in the C2S_Login case above).
                if (req->use_type() == ItemUseType::Equip)
                    weaponType_ = req->slot();

                // Broadcast generically for every use_type; the client
                // decides what (if anything) to do with each type. Currently
                // Reload and Equip are consumed by the client to mirror the
                // reload motion / held-weapon visual.
                flatbuffers::FlatBufferBuilder fbb;
                auto broadcast = CreateS2C_ItemUseBroadcast(fbb, id_, req->use_type(), req->slot());
                auto reply = CreatePacket(fbb, Payload::S2C_ItemUseBroadcast, broadcast.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_SaveInventory:
            {
                // No reply, no broadcast -- this is purely "persist my
                // current grid", not something other players need to know
                // about (unlike position/weapon/attack). Silently ignored
                // for anonymous (non-account) sessions, same as MoveInput's
                // progress save.
                if (accountId_ < 0)
                    break;

                const auto* req = packet->payload_as_C2S_SaveInventory();
                if (!req || !req->items())
                    break;

                std::vector<InventoryItemRecord> items;
                items.reserve(req->items()->size());
                for (const auto* entry : *req->items())
                {
                    if (!entry || !entry->item_id())
                        continue;
                    InventoryItemRecord record;
                    record.itemId = entry->item_id()->str();
                    record.gridX = entry->grid_x();
                    record.gridY = entry->grid_y();
                    record.rotated = entry->rotated();
                    record.stackCount = entry->stack_count();
                    items.push_back(std::move(record));
                }

                Database::Get().SaveInventory(accountId_, items);
                break;
            }

            default:
                break;
        }
    }

    void Session::ResolveAndBroadcastHit(const ProtoType::Net::Vec3& origin, const ProtoType::Net::Vec3& direction, uint8_t weaponSlot)
    {
        using namespace ProtoType::Net;

        constexpr float kHitRadius = 150.0f;
        constexpr float kMaxRange = 10000.0f;
        constexpr float kDamagePerHit = 20.0f;

        const float dirLenSq = direction.x() * direction.x() + direction.y() * direction.y() + direction.z() * direction.z();
        if (dirLenSq < 0.0001f)
            return;

        const float dirLen = std::sqrt(dirLenSq);
        const float dx = direction.x() / dirLen;
        const float dy = direction.y() / dirLen;
        const float dz = direction.z() / dirLen;

        bool found = false;
        float bestT = kMaxRange;
        uint32_t bestTargetId = 0;
        Vec3 bestPosition(0.0f, 0.0f, 0.0f);

        for (const auto& other : server_.SnapshotOtherSessions(id_))
        {
            const Vec3 pos = other->GetPosition();
            const float toX = pos.x() - origin.x();
            const float toY = pos.y() - origin.y();
            const float toZ = pos.z() - origin.z();

            const float t = toX * dx + toY * dy + toZ * dz;
            if (t < 0.0f || t > kMaxRange)
                continue;

            const float closestX = origin.x() + dx * t;
            const float closestY = origin.y() + dy * t;
            const float closestZ = origin.z() + dz * t;

            const float distX = pos.x() - closestX;
            const float distY = pos.y() - closestY;
            const float distZ = pos.z() - closestZ;
            const float distSq = distX * distX + distY * distY + distZ * distZ;

            if (distSq <= kHitRadius * kHitRadius && t < bestT)
            {
                found = true;
                bestT = t;
                bestTargetId = other->GetId();
                bestPosition = pos;
            }
        }

        flatbuffers::FlatBufferBuilder fbb;
        auto result = CreateS2C_AttackResult(fbb, /*server_tick*/ 0, id_, bestTargetId,
            /*weapon_id*/ static_cast<uint32_t>(weaponSlot), found, found ? &bestPosition : nullptr,
            HitBone::None, found ? kDamagePerHit : 0.0f);
        auto reply = CreatePacket(fbb, Payload::S2C_AttackResult, result.Union());
        FinishSizePrefixedPacketBuffer(fbb, reply);

        EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()), static_cast<uint32_t>(fbb.GetSize()));
        server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()), static_cast<uint32_t>(fbb.GetSize()));
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
            // roster/join broadcast, or S2C_MoveState broadcast) above;
            // SaveInventory intentionally gets no reply at all (see its case
            // above). Self-echoing any of their raw C2S_* packets back would
            // just be stream noise that could be mistaken for a real S2C_*
            // message.
            const auto type = packet->payload_type();
            const bool skipSelfEcho =
                (type == ProtoType::Net::Payload::C2S_Login ||
                 type == ProtoType::Net::Payload::C2S_MoveInput ||
                 type == ProtoType::Net::Payload::C2S_SaveInventory);
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

    void Session::FlushProgress()
    {
        if (accountId_ < 0)
            return;

        Database::Get().SaveProgress(accountId_, position_, look_, weaponType_);
    }
}
