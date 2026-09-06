#include "Session.h"
#include "EchoServer.h"
#include "EnemyAI.h"
#include "HitDetection.h"
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

            case Payload::C2S_SaveEquipment:
                return "save equipment";

            case Payload::C2S_SaveQuickSlots:
                return "save quick slots";

            case Payload::C2S_SetVisible:
                if (const auto* req = packet->payload_as_C2S_SetVisible())
                    return req->visible() ? "become visible" : "become invisible";
                return "set visible";

            case Payload::C2S_PlayerDied:
                return "player died";

            case Payload::C2S_ContainerLootRoll:
                return "container loot roll";

            case Payload::C2S_CompanionMoveInput:
                return "companion move";

            case Payload::C2S_EnemyClaimRequest:
                return "enemy claim request";

            case Payload::C2S_EnemyState:
                return "enemy state";

            case Payload::C2S_EnemyDamage:
                return "enemy damage";

            case Payload::C2S_EnemyRegister:
                return "enemy register";

            case Payload::C2S_ItemSpawnRoll:
                return "item spawn roll";

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
                std::vector<EquipmentItemRecord> savedEquipment;
                std::vector<QuickSlotItemRecord> savedQuickSlots;

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
                    Database::Get().LoadEquipment(accountId, savedEquipment);
                    Database::Get().LoadQuickSlots(accountId, savedQuickSlots);
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

                    std::vector<flatbuffers::Offset<EquipmentItemEntry>> equipmentOffsets;
                    equipmentOffsets.reserve(savedEquipment.size());
                    for (const auto& item : savedEquipment)
                    {
                        auto itemIdOffset = fbb.CreateString(item.itemId);
                        equipmentOffsets.push_back(CreateEquipmentItemEntry(fbb, item.slot, itemIdOffset));
                    }
                    auto equipmentVector = fbb.CreateVector(equipmentOffsets);

                    std::vector<flatbuffers::Offset<QuickSlotItemEntry>> quickSlotOffsets;
                    quickSlotOffsets.reserve(savedQuickSlots.size());
                    for (const auto& item : savedQuickSlots)
                    {
                        auto itemIdOffset = fbb.CreateString(item.itemId);
                        quickSlotOffsets.push_back(CreateQuickSlotItemEntry(fbb, item.slotIndex, itemIdOffset, item.stackCount));
                    }
                    auto quickSlotVector = fbb.CreateVector(quickSlotOffsets);

                    auto success = CreateS2C_LoginSuccess(fbb, id_, &position_, &look_, weaponType_, hasSavedProgress,
                        inventoryVector, equipmentVector, quickSlotVector);
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

                // 2b) Tell this client about every door someone already
                // toggled this server run (see EchoServer::SetDoorState's
                // header comment for why this is world state, not
                // per-session state like the roster above). Doors never
                // toggled have no entry -- closed is what every client
                // already spawns with, nothing to replay. player_id here is
                // meaningless (the client's OnDoorInteract handler only
                // looks at target_id/interact_type), so it's just this
                // session's own id rather than whoever actually toggled it
                // (that information isn't kept, only the current state is).
                for (const auto& [doorId, isOpen] : server_.SnapshotDoorStates())
                {
                    flatbuffers::FlatBufferBuilder doorFbb;
                    auto doorResult = CreateS2C_InteractResult(doorFbb, id_, doorId,
                        isOpen ? InteractType::DoorOpen : InteractType::DoorClose, ResultCode::Ok);
                    auto doorReply = CreatePacket(doorFbb, Payload::S2C_InteractResult, doorResult.Union());
                    FinishSizePrefixedPacketBuffer(doorFbb, doorReply);
                    EnqueueEcho(reinterpret_cast<const char*>(doorFbb.GetBufferPointer()),
                                static_cast<uint32_t>(doorFbb.GetSize()));
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

            case Payload::C2S_SaveEquipment:
            {
                // Same trust tier as C2S_SaveInventory above -- no reply, no
                // broadcast, silently ignored for guest sessions.
                if (accountId_ < 0)
                    break;

                const auto* req = packet->payload_as_C2S_SaveEquipment();
                if (!req || !req->items())
                    break;

                std::vector<EquipmentItemRecord> items;
                items.reserve(req->items()->size());
                for (const auto* entry : *req->items())
                {
                    if (!entry || !entry->item_id())
                        continue;
                    EquipmentItemRecord record;
                    record.slot = entry->slot();
                    record.itemId = entry->item_id()->str();
                    items.push_back(std::move(record));
                }

                Database::Get().SaveEquipment(accountId_, items);
                break;
            }

            case Payload::C2S_SaveQuickSlots:
            {
                // Same trust tier as C2S_SaveInventory above -- no reply, no
                // broadcast, silently ignored for guest sessions.
                if (accountId_ < 0)
                    break;

                const auto* req = packet->payload_as_C2S_SaveQuickSlots();
                if (!req || !req->items())
                    break;

                std::vector<QuickSlotItemRecord> items;
                items.reserve(req->items()->size());
                for (const auto* entry : *req->items())
                {
                    if (!entry || !entry->item_id())
                        continue;
                    QuickSlotItemRecord record;
                    record.slotIndex = entry->slot_index();
                    record.itemId = entry->item_id()->str();
                    record.stackCount = entry->stack_count();
                    items.push_back(std::move(record));
                }

                Database::Get().SaveQuickSlots(accountId_, items);
                break;
            }

            case Payload::C2S_SetVisible:
            {
                const auto* req = packet->payload_as_C2S_SetVisible();
                if (!req)
                    break;

                // visible=true needs no reply here -- see this message's
                // schema comment: the next C2S_MoveInput this session sends
                // (resumed once the client's back in a Multi map) naturally
                // respawns it on everyone else's screen. visible=false is
                // the only direction that needs us to do anything: broadcast
                // the exact same "player left" message a real disconnect
                // would, on this still-connected session's behalf, so other
                // clients despawn it instead of freezing it in place.
                if (!req->visible())
                {
                    flatbuffers::FlatBufferBuilder fbb;
                    auto left = CreateS2C_PlayerLeft(fbb, id_);
                    auto reply = CreatePacket(fbb, Payload::S2C_PlayerLeft, left.Union());
                    FinishSizePrefixedPacketBuffer(fbb, reply);
                    server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                       static_cast<uint32_t>(fbb.GetSize()));
                }
                break;
            }

            case Payload::C2S_PlayerDied:
            {
                // No fields to validate -- player_id is always id_ (see
                // this message's schema comment), so there's nothing this
                // session could get wrong here the way a claimed id could.
                flatbuffers::FlatBufferBuilder fbb;
                auto died = CreateS2C_PlayerDied(fbb, id_);
                auto reply = CreatePacket(fbb, Payload::S2C_PlayerDied, died.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_InteractRequest:
            {
                const auto* req = packet->payload_as_C2S_InteractRequest();
                if (!req)
                    break;

                // Loot (picking up a ground ADropItem) needs arbitration,
                // not just a relay: target_id here is the item's NetSlotId
                // (stable across every client's copy of the same spawned
                // item -- see DropItem.h), and two players could interact
                // with the same ground item in the same instant. First
                // claim wins (ClaimItemPickup); the winner's result is
                // broadcast to everyone (including themselves) so every
                // client destroys its copy of that item, while a loser gets
                // told Denied and just destroys its own copy without adding
                // anything to their inventory.
                if (req->interact_type() == InteractType::Loot)
                {
                    const bool granted = server_.ClaimItemPickup(req->target_id(), id_);
                    const ResultCode result = granted ? ResultCode::Ok : ResultCode::Denied;

                    flatbuffers::FlatBufferBuilder fbb;
                    auto pickupResult = CreateS2C_InteractResult(fbb, id_, req->target_id(), req->interact_type(), result);
                    auto reply = CreatePacket(fbb, Payload::S2C_InteractResult, pickupResult.Union());
                    FinishSizePrefixedPacketBuffer(fbb, reply);

                    EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                static_cast<uint32_t>(fbb.GetSize()));
                    if (granted)
                    {
                        server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                           static_cast<uint32_t>(fbb.GetSize()));
                    }
                    break;
                }

                // Only door open/close is wired up beyond Loot above --
                // Extract/PlantItem/UseSwitch have no server-side meaning
                // yet. Simple relay, no arbitration: unlike a loot roll (or
                // Loot pickup above) there's no "right answer" to agree on
                // here, just "player X toggled door Y, everyone else's copy
                // should match" -- same trust tier as S2C_ItemUseBroadcast's
                // weapon equip/reload relay. The toggle IS recorded
                // (SetDoorState) purely so a client joining later gets it
                // replayed in C2S_Login's roster loop -- see that loop and
                // EchoServer::SetDoorState's header comment.
                if (req->interact_type() != InteractType::DoorOpen && req->interact_type() != InteractType::DoorClose)
                    break;

                server_.SetDoorState(req->target_id(), req->interact_type() == InteractType::DoorOpen);

                flatbuffers::FlatBufferBuilder fbb;
                auto result = CreateS2C_InteractResult(fbb, id_, req->target_id(), req->interact_type(), ResultCode::Ok);
                auto reply = CreatePacket(fbb, Payload::S2C_InteractResult, result.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_ItemSpawnRoll:
            {
                const auto* req = packet->payload_as_C2S_ItemSpawnRoll();
                if (!req)
                    break;

                std::vector<WorldItemRecord> proposed;
                if (req->items())
                {
                    proposed.reserve(req->items()->size());
                    for (const auto* entry : *req->items())
                    {
                        if (!entry || !entry->item_id())
                            continue;
                        WorldItemRecord record;
                        record.itemId = entry->item_id()->str();
                        if (entry->position())
                        {
                            record.posX = entry->position()->x();
                            record.posY = entry->position()->y();
                            record.posZ = entry->position()->z();
                        }
                        record.stackCount = entry->stack_count();
                        proposed.push_back(std::move(record));
                    }
                }

                // First roll for this spawn_point_id wins -- same
                // first-roll-wins arbitration as C2S_ContainerLootRoll,
                // just for loose world drops instead of a grid container.
                const std::vector<WorldItemRecord>& authoritative =
                    server_.ClaimItemSpawnRoll(req->spawn_point_id(), std::move(proposed));

                flatbuffers::FlatBufferBuilder fbb;
                std::vector<flatbuffers::Offset<WorldSpawnedItemEntry>> itemOffsets;
                itemOffsets.reserve(authoritative.size());
                for (const auto& item : authoritative)
                {
                    auto itemIdOffset = fbb.CreateString(item.itemId);
                    const Vec3 position(item.posX, item.posY, item.posZ);
                    itemOffsets.push_back(CreateWorldSpawnedItemEntry(fbb, itemIdOffset, &position, item.stackCount));
                }
                auto itemsVector = fbb.CreateVector(itemOffsets);
                auto state = CreateS2C_ItemSpawnState(fbb, req->spawn_point_id(), itemsVector);
                auto reply = CreatePacket(fbb, Payload::S2C_ItemSpawnState, state.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);

                EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                            static_cast<uint32_t>(fbb.GetSize()));
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_ContainerLootRoll:
            {
                const auto* req = packet->payload_as_C2S_ContainerLootRoll();
                if (!req)
                    break;

                std::vector<InventoryItemRecord> proposed;
                if (req->items())
                {
                    proposed.reserve(req->items()->size());
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
                        proposed.push_back(std::move(record));
                    }
                }

                // First roll for this container_id wins; everyone (including
                // this sender, whose own roll may just have been rejected in
                // favor of an earlier one) gets told the same answer.
                const std::vector<InventoryItemRecord>& authoritative =
                    server_.ClaimContainerLoot(req->container_id(), std::move(proposed));

                flatbuffers::FlatBufferBuilder fbb;
                std::vector<flatbuffers::Offset<InventoryItemEntry>> itemOffsets;
                itemOffsets.reserve(authoritative.size());
                for (const auto& item : authoritative)
                {
                    auto itemIdOffset = fbb.CreateString(item.itemId);
                    itemOffsets.push_back(CreateInventoryItemEntry(
                        fbb, itemIdOffset, item.gridX, item.gridY, item.rotated, item.stackCount));
                }
                auto itemsVector = fbb.CreateVector(itemOffsets);
                auto state = CreateS2C_ContainerLootState(fbb, req->container_id(), itemsVector);
                auto reply = CreatePacket(fbb, Payload::S2C_ContainerLootState, state.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);

                EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                            static_cast<uint32_t>(fbb.GetSize()));
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_CompanionMoveInput:
            {
                const auto* req = packet->payload_as_C2S_CompanionMoveInput();
                if (!req)
                    break;

                // No separate companion login/session -- owner_id is this
                // session's own id_, the same "server decides, not the
                // client" rule C2S_MoveInput follows for player_id.
                const Vec3 position = req->position() ? *req->position() : Vec3(0.0f, 0.0f, 0.0f);
                const Rotator look = req->look() ? *req->look() : Rotator(0.0f, 0.0f, 0.0f);

                flatbuffers::FlatBufferBuilder fbb;
                auto state = CreateS2C_CompanionMoveState(fbb, id_, &position, &look, req->health(), req->is_dead(),
                    req->weapon_type(), req->is_aiming(), req->aim_pitch());
                auto reply = CreatePacket(fbb, Payload::S2C_CompanionMoveState, state.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_EnemyClaimRequest:
            {
                const auto* req = packet->payload_as_C2S_EnemyClaimRequest();
                if (!req)
                    break;

                const bool granted = server_.ClaimEnemy(req->enemy_id(), id_);

                flatbuffers::FlatBufferBuilder fbb;
                auto result = CreateS2C_EnemyClaimResult(fbb, req->enemy_id(), granted);
                auto reply = CreatePacket(fbb, Payload::S2C_EnemyClaimResult, result.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                EnqueueEcho(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                            static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_EnemyState:
            {
                const auto* req = packet->payload_as_C2S_EnemyState();
                if (!req)
                    break;

                // Trusted as-is, same as C2S_MoveInput's position -- a
                // non-owner sending this would just be overwritten by the
                // real owner's next update anyway.
                flatbuffers::FlatBufferBuilder fbb;
                const Vec3 position = req->position() ? *req->position() : Vec3(0.0f, 0.0f, 0.0f);
                const Rotator look = req->look() ? *req->look() : Rotator(0.0f, 0.0f, 0.0f);
                auto state = CreateS2C_EnemyState(fbb, req->enemy_id(), &position, &look, req->health(), req->is_dead());
                auto reply = CreatePacket(fbb, Payload::S2C_EnemyState, state.Union());
                FinishSizePrefixedPacketBuffer(fbb, reply);
                server_.Broadcast(id_, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                   static_cast<uint32_t>(fbb.GetSize()));
                break;
            }

            case Payload::C2S_EnemyDamage:
            {
                const auto* req = packet->payload_as_C2S_EnemyDamage();
                if (!req)
                    break;

                // Server-driven enemies (registered via C2S_EnemyRegister,
                // Multi map only) apply damage directly -- the server IS
                // the health authority for those, there's no owner to relay
                // to. Falls back to the older client-ownership relay for
                // everything else (Single map, or not yet registered).
                if (!server_.ApplyServerEnemyDamage(req->enemy_id(), req->damage()))
                {
                    // Unicast to the current owner only -- see this message's
                    // schema comment. Silently dropped if the enemy is
                    // unclaimed or its owner already disconnected; the next
                    // client to claim it starts from whatever health the last
                    // owner had broadcast.
                    if (auto ownerSession = server_.FindEnemyOwnerSession(req->enemy_id()))
                    {
                        flatbuffers::FlatBufferBuilder fbb;
                        auto damage = CreateS2C_EnemyDamage(fbb, req->enemy_id(), req->damage());
                        auto reply = CreatePacket(fbb, Payload::S2C_EnemyDamage, damage.Union());
                        FinishSizePrefixedPacketBuffer(fbb, reply);
                        ownerSession->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                            static_cast<uint32_t>(fbb.GetSize()));
                    }
                }
                break;
            }

            case Payload::C2S_EnemyRegister:
            {
                const auto* req = packet->payload_as_C2S_EnemyRegister();
                if (!req)
                    break;

                // No reply -- unlike C2S_EnemyClaimRequest, the registering
                // client doesn't wait to find out if it "won" anything; it
                // switches to mirroring S2C_EnemyState immediately (see
                // AEnemyBase's multiplayer-map branch). The first
                // registration for a given enemy_id wins its starting stats.
                FEnemyAiRecord initial;
                if (req->position())
                {
                    initial.posX = req->position()->x();
                    initial.posY = req->position()->y();
                    initial.posZ = req->position()->z();
                }
                initial.health = req->health();
                initial.maxHealth = req->max_health();
                initial.moveSpeed = req->move_speed();
                initial.attackRange = req->attack_range();
                initial.attackDamage = req->attack_damage();
                initial.attackCooldown = req->attack_cooldown();
                initial.isCaller = req->is_caller();
                initial.callRadius = req->call_radius();
                initial.callCooldown = req->call_cooldown();
                server_.RegisterServerEnemy(req->enemy_id(), initial);
                break;
            }

            default:
                break;
        }
    }

    void Session::ResolveAndBroadcastHit(const ProtoType::Net::Vec3& origin, const ProtoType::Net::Vec3& direction, uint8_t weaponSlot)
    {
        using namespace ProtoType::Net;

        constexpr float kMaxRange = 10000.0f;
        constexpr float kDamagePerHit = 20.0f;

        const float dirLenSq = direction.x() * direction.x() + direction.y() * direction.y() + direction.z() * direction.z();
        if (dirLenSq < 0.0001f)
            return;

        const float dirLen = std::sqrt(dirLenSq);
        const Vec3 dir(direction.x() / dirLen, direction.y() / dirLen, direction.z() / dirLen);

        bool found = false;
        float bestT = kMaxRange;
        uint32_t bestTargetId = 0;
        Vec3 bestPosition(0.0f, 0.0f, 0.0f);
        HitBone bestBone = HitBone::None;

        for (const auto& other : server_.SnapshotOtherSessions(id_))
        {
            // Real hitbox test (capsule, not a flat sphere blob) -- see
            // HitDetection.h for what this approximates and why.
            const FHitResult hitResult = TestRayAgainstPlayerCapsule(
                origin, dir, other->GetPosition(), other->GetLook().yaw(), kMaxRange);

            if (hitResult.hit && hitResult.rayT < bestT)
            {
                found = true;
                bestT = hitResult.rayT;
                bestTargetId = other->GetId();
                bestPosition = hitResult.hitPosition;
                bestBone = hitResult.hitBone;
            }
        }

        flatbuffers::FlatBufferBuilder fbb;
        auto result = CreateS2C_AttackResult(fbb, /*server_tick*/ 0, id_, bestTargetId,
            /*weapon_id*/ static_cast<uint32_t>(weaponSlot), found, found ? &bestPosition : nullptr,
            bestBone, found ? kDamagePerHit : 0.0f);
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
            // SaveInventory/SetVisible intentionally get no reply at all
            // (see their cases above). Self-echoing any of their raw C2S_*
            // packets back would just be stream noise that could be mistaken
            // for a real S2C_* message.
            const auto type = packet->payload_type();
            const bool skipSelfEcho =
                (type == ProtoType::Net::Payload::C2S_Login ||
                 type == ProtoType::Net::Payload::C2S_MoveInput ||
                 type == ProtoType::Net::Payload::C2S_SaveInventory ||
                 type == ProtoType::Net::Payload::C2S_SaveEquipment ||
                 type == ProtoType::Net::Payload::C2S_SaveQuickSlots ||
                 type == ProtoType::Net::Payload::C2S_SetVisible ||
                 type == ProtoType::Net::Payload::C2S_ContainerLootRoll ||
                 type == ProtoType::Net::Payload::C2S_CompanionMoveInput ||
                 type == ProtoType::Net::Payload::C2S_EnemyClaimRequest ||
                 type == ProtoType::Net::Payload::C2S_EnemyState ||
                 type == ProtoType::Net::Payload::C2S_EnemyDamage ||
                 type == ProtoType::Net::Payload::C2S_EnemyRegister ||
                 type == ProtoType::Net::Payload::C2S_InteractRequest ||
                 type == ProtoType::Net::Payload::C2S_ItemSpawnRoll ||
                 type == ProtoType::Net::Payload::C2S_PlayerDied);
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
