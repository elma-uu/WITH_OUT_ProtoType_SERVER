#include "Room.h"
#include "Session.h"
#include "packet.h"

namespace Wop
{
    /*-------------------
     멤버십
    -------------------*/
    void Room::AddSession(std::shared_ptr<Session> session)
    {
        if (!session)
            return;

        session->SetRoom(shared_from_this());

        {
            std::lock_guard<std::mutex> guard(membersLock_);
            members_.emplace(session->GetId(), session);
        }

        AnnounceNewMember(session);
    }

    void Room::AnnounceNewMember(const std::shared_ptr<Session>& newMember)
    {
        using namespace ProtoType::Net;

        // 1) Tell the new member about everyone already in this room
        // (SnapshotOtherSessions already excludes newMember by id, whether
        // or not it's been inserted into members_ yet).
        for (const auto& other : SnapshotOtherSessions(newMember->GetId()))
        {
            flatbuffers::FlatBufferBuilder fbb;
            const std::string nickname = "Player" + std::to_string(other->GetId());
            auto nicknameOffset = fbb.CreateString(nickname);
            const Vec3 otherPos = other->GetPosition();
            const Rotator otherLook = other->GetLook();
            auto info = CreateS2C_SendPlayerInfo(fbb, other->GetId(), nicknameOffset, &otherPos, &otherLook, 0, 0);
            auto reply = CreatePacket(fbb, Payload::S2C_SendPlayerInfo, info.Union());
            FinishSizePrefixedPacketBuffer(fbb, reply);
            newMember->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                             static_cast<uint32_t>(fbb.GetSize()));

            // Also tell the new member what this existing player is
            // currently holding, so their weapon shows up right away
            // instead of only on their next weapon swap.
            if (const uint8_t otherWeaponType = other->GetWeaponType(); otherWeaponType != 0)
            {
                flatbuffers::FlatBufferBuilder equipFbb;
                auto equip = CreateS2C_ItemUseBroadcast(equipFbb, other->GetId(), ItemUseType::Equip, otherWeaponType);
                auto equipReply = CreatePacket(equipFbb, Payload::S2C_ItemUseBroadcast, equip.Union());
                FinishSizePrefixedPacketBuffer(equipFbb, equipReply);
                newMember->Send(reinterpret_cast<const char*>(equipFbb.GetBufferPointer()),
                                 static_cast<uint32_t>(equipFbb.GetSize()));
            }
        }

        // 2) Tell the new member about every door someone already toggled
        // in this room this run (see SetDoorState's header comment for why
        // this is room state, not per-session state like the roster
        // above). Doors never toggled have no entry -- closed is what
        // every client already spawns with, nothing to replay.
        for (const auto& [doorId, isOpen] : SnapshotDoorStates())
        {
            flatbuffers::FlatBufferBuilder doorFbb;
            auto doorResult = CreateS2C_InteractResult(doorFbb, newMember->GetId(), doorId,
                isOpen ? InteractType::DoorOpen : InteractType::DoorClose, ResultCode::Ok);
            auto doorReply = CreatePacket(doorFbb, Payload::S2C_InteractResult, doorResult.Union());
            FinishSizePrefixedPacketBuffer(doorFbb, doorReply);
            newMember->Send(reinterpret_cast<const char*>(doorFbb.GetBufferPointer()),
                             static_cast<uint32_t>(doorFbb.GetSize()));
        }

        // 3) Tell everyone else in this room that the new member just
        // joined.
        {
            flatbuffers::FlatBufferBuilder fbb;
            const std::string nickname = "Player" + std::to_string(newMember->GetId());
            auto nicknameOffset = fbb.CreateString(nickname);
            const Vec3 pos = newMember->GetPosition();
            const Rotator look = newMember->GetLook();
            auto info = CreateS2C_SendPlayerInfo(fbb, newMember->GetId(), nicknameOffset, &pos, &look, 0, 0);
            auto reply = CreatePacket(fbb, Payload::S2C_SendPlayerInfo, info.Union());
            FinishSizePrefixedPacketBuffer(fbb, reply);
            Broadcast(newMember->GetId(), reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                      static_cast<uint32_t>(fbb.GetSize()));
        }

        // If restored progress means the new member already has a weapon
        // out, tell everyone else too (mirrors step 1 above, just in the
        // other direction).
        if (const uint8_t weaponType = newMember->GetWeaponType(); weaponType != 0)
        {
            flatbuffers::FlatBufferBuilder equipFbb;
            auto equip = CreateS2C_ItemUseBroadcast(equipFbb, newMember->GetId(), ItemUseType::Equip, weaponType);
            auto equipReply = CreatePacket(equipFbb, Payload::S2C_ItemUseBroadcast, equip.Union());
            FinishSizePrefixedPacketBuffer(equipFbb, equipReply);
            Broadcast(newMember->GetId(), reinterpret_cast<const char*>(equipFbb.GetBufferPointer()),
                      static_cast<uint32_t>(equipFbb.GetSize()));
        }
    }

    void Room::RemoveSession(uint32_t sessionId)
    {
        std::lock_guard<std::mutex> guard(membersLock_);
        members_.erase(sessionId);
    }

    size_t Room::MemberCount() const
    {
        std::lock_guard<std::mutex> guard(membersLock_);
        return members_.size();
    }

    /*-------------------
     브로드캐스트
    -------------------*/
    void Room::Broadcast(uint32_t excludeSessionId, const char* data, uint32_t len)
    {
        for (const auto& session : SnapshotOtherSessions(excludeSessionId))
            session->Send(data, len);
    }

    std::vector<std::shared_ptr<Session>> Room::SnapshotOtherSessions(uint32_t excludeSessionId)
    {
        std::vector<std::shared_ptr<Session>> result;
        std::lock_guard<std::mutex> guard(membersLock_);
        result.reserve(members_.size());
        for (const auto& [id, session] : members_)
        {
            if (id != excludeSessionId)
                result.push_back(session);
        }
        return result;
    }

    /*-------------------
     월드 아이템 상태
    -------------------*/
    const std::vector<InventoryItemRecord>& Room::ClaimContainerLoot(
        uint32_t containerId, std::vector<InventoryItemRecord> proposed)
    {
        std::lock_guard<std::mutex> guard(containerLootLock_);
        // try_emplace only constructs/inserts `proposed` if containerId isn't
        // already present -- if it is, the existing entry (an earlier
        // client's roll) is left untouched and returned instead.
        const auto [it, inserted] = containerLoot_.try_emplace(containerId, std::move(proposed));
        return it->second;
    }

    const std::vector<WorldItemRecord>& Room::ClaimItemSpawnRoll(
        uint32_t spawnPointId, std::vector<WorldItemRecord> proposed)
    {
        std::lock_guard<std::mutex> guard(itemSpawnLock_);
        const auto [it, inserted] = itemSpawnRolls_.try_emplace(spawnPointId, std::move(proposed));
        return it->second;
    }

    /*-------------------
     문 상태
    -------------------*/
    void Room::SetDoorState(uint32_t doorId, bool isOpen)
    {
        std::lock_guard<std::mutex> guard(doorStateLock_);
        doorStates_[doorId] = isOpen;
    }

    std::vector<std::pair<uint32_t, bool>> Room::SnapshotDoorStates() const
    {
        std::lock_guard<std::mutex> guard(doorStateLock_);
        std::vector<std::pair<uint32_t, bool>> snapshot;
        snapshot.reserve(doorStates_.size());
        for (const auto& [doorId, isOpen] : doorStates_)
            snapshot.emplace_back(doorId, isOpen);
        return snapshot;
    }

    bool Room::ClaimItemPickup(uint32_t netSlotId, uint32_t sessionId)
    {
        std::lock_guard<std::mutex> guard(itemPickupLock_);
        const auto [it, inserted] = pickedUpItems_.try_emplace(netSlotId, sessionId);
        return inserted;
    }

    /*-------------------
     적(좀비) AI 소유권 (레거시 클라이언트-주도 경로)
    -------------------*/
    bool Room::ClaimEnemy(uint32_t enemyId, uint32_t sessionId)
    {
        std::lock_guard<std::mutex> guard(enemyOwnerLock_);
        const auto [it, inserted] = enemyOwners_.try_emplace(enemyId, sessionId);
        return inserted || it->second == sessionId;
    }

    std::vector<uint32_t> Room::ReleaseEnemiesOwnedBy(uint32_t sessionId)
    {
        std::vector<uint32_t> released;
        std::lock_guard<std::mutex> guard(enemyOwnerLock_);
        for (auto it = enemyOwners_.begin(); it != enemyOwners_.end(); )
        {
            if (it->second == sessionId)
            {
                released.push_back(it->first);
                it = enemyOwners_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return released;
    }

    std::shared_ptr<Session> Room::FindEnemyOwnerSession(uint32_t enemyId)
    {
        uint32_t ownerId = 0;
        {
            std::lock_guard<std::mutex> guard(enemyOwnerLock_);
            const auto it = enemyOwners_.find(enemyId);
            if (it == enemyOwners_.end())
                return nullptr;
            ownerId = it->second;
        }

        std::lock_guard<std::mutex> guard(membersLock_);
        const auto it = members_.find(ownerId);
        return it != members_.end() ? it->second : nullptr;
    }

    /*-------------------
     서버 권위 적(좀비) AI
    -------------------*/
    void Room::LoadEnemyObstacles(const std::string& path)
    {
        enemyAi_.LoadObstacles(path);
    }

    void Room::RegisterServerEnemy(uint32_t enemyId, const FEnemyAiRecord& initial)
    {
        enemyAi_.RegisterIfNew(enemyId, initial);
    }

    bool Room::ApplyServerEnemyDamage(uint32_t enemyId, float damage)
    {
        return enemyAi_.ApplyDamage(enemyId, damage);
    }

    void Room::Tick(float deltaSeconds)
    {
        using namespace ProtoType::Net;

        // Session id 0 never belongs to a real connection, so this
        // snapshots every member of this room -- there's no single
        // "excluded" session for server-driven AI the way there is for a
        // player's own broadcast. Invisible members (Session::IsVisible ==
        // false, i.e. left this room's Multi map for a Single map/hub while
        // staying connected) are skipped: their position_ stopped updating
        // the moment they left, so leaving them in would let a zombie keep
        // "chasing"/landing hits on a frozen, stale position from a map
        // that player isn't even in anymore.
        std::vector<FEnemyAiPlayerSnapshot> playerSnapshots;
        for (const auto& session : SnapshotOtherSessions(0))
        {
            if (!session->IsVisible())
                continue;
            const Vec3 pos = session->GetPosition();
            playerSnapshots.push_back({ session->GetId(), pos.x(), pos.y(), pos.z() });
        }

        const FEnemyTickResult tickResult = enemyAi_.Tick(deltaSeconds, playerSnapshots);

        for (const auto& update : tickResult.stateUpdates)
        {
            flatbuffers::FlatBufferBuilder fbb;
            const Vec3 position(update.record.posX, update.record.posY, update.record.posZ);
            const Rotator look(0.0f, update.record.lookYaw, 0.0f);
            auto state = CreateS2C_EnemyState(
                fbb, update.enemyId, &position, &look, update.record.health, update.record.isDead);
            auto packet = CreatePacket(fbb, Payload::S2C_EnemyState, state.Union());
            FinishSizePrefixedPacketBuffer(fbb, packet);
            Broadcast(0, reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                      static_cast<uint32_t>(fbb.GetSize()));
        }

        for (const auto& attack : tickResult.attackEvents)
        {
            // Unicast, same trust tier as S2C_AttackResult -- the server
            // decided this hit happened and how much it's worth, but the
            // target's own client applies it to its own health (see
            // S2C_EnemyAttackResult's schema comment).
            std::shared_ptr<Session> targetSession;
            {
                std::lock_guard<std::mutex> guard(membersLock_);
                const auto it = members_.find(attack.targetSessionId);
                if (it != members_.end())
                    targetSession = it->second;
            }
            if (!targetSession)
                continue;

            flatbuffers::FlatBufferBuilder fbb;
            auto result = CreateS2C_EnemyAttackResult(
                fbb, attack.enemyId, attack.targetSessionId, attack.damage);
            auto packet = CreatePacket(fbb, Payload::S2C_EnemyAttackResult, result.Union());
            FinishSizePrefixedPacketBuffer(fbb, packet);
            targetSession->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                 static_cast<uint32_t>(fbb.GetSize()));

            // Everyone else's mirrored copy of enemyId needs to see the
            // swing too, not just the target's health drop -- see
            // S2C_EnemyAttackBroadcast's schema comment.
            flatbuffers::FlatBufferBuilder broadcastFbb;
            auto broadcastMsg = CreateS2C_EnemyAttackBroadcast(broadcastFbb, attack.enemyId);
            auto broadcastPacket = CreatePacket(
                broadcastFbb, Payload::S2C_EnemyAttackBroadcast, broadcastMsg.Union());
            FinishSizePrefixedPacketBuffer(broadcastFbb, broadcastPacket);
            Broadcast(0, reinterpret_cast<const char*>(broadcastFbb.GetBufferPointer()),
                      static_cast<uint32_t>(broadcastFbb.GetSize()));
        }
    }

    void Room::ResetWorldStateIfNoVisibleMembers()
    {
        // Session id 0 never belongs to a real connection -- same "snapshot
        // everyone" idiom Tick() uses.
        for (const auto& session : SnapshotOtherSessions(0))
        {
            if (session->IsVisible())
            {
                // Someone's still in this room's Multi map -- their loot/
                // doors/enemies are still "the current game", not stale.
                return;
            }
        }

        // Forget every claim this room has accumulated for container loot/
        // item spawns/pickups/doors/enemies, so the next player(s) to enter
        // this room's Multi map start a genuinely fresh world instead of one
        // where, say, half the loot from an earlier visit is permanently
        // already-claimed (see ClaimItemPickup/ClaimContainerLoot/
        // ClaimItemSpawnRoll -- these have no expiry, a claimed slot stays
        // claimed for this Room's whole lifetime otherwise) or every zombie
        // someone killed earlier is still registered as dead with nothing
        // left to fight on the next visit. Doesn't touch per-account DB
        // state (progress/inventory) -- only this in-memory, not-tied-to-
        // any-account world state.
        {
            std::lock_guard<std::mutex> guard(itemPickupLock_);
            pickedUpItems_.clear();
        }
        {
            std::lock_guard<std::mutex> guard(itemSpawnLock_);
            itemSpawnRolls_.clear();
        }
        {
            std::lock_guard<std::mutex> guard(containerLootLock_);
            containerLoot_.clear();
        }
        {
            std::lock_guard<std::mutex> guard(doorStateLock_);
            doorStates_.clear();
        }
        enemyAi_.Reset();
        std::printf("[Room %u] empty (no visible members left) -- world state (loot/pickups/doors/enemies) reset for the next visit.\n", roomId_);
    }
}
