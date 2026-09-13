#pragma once
#include "Database.h"
#include "EnemyAI.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wop
{
    class Session;

    // One loose item dropped in the open world by an AItemSpawnPoint's
    // roll -- see Room::ClaimItemSpawnRoll. Distinct from
    // InventoryItemRecord (Database.h): that one is a grid-slot entry,
    // this is a fixed world position, and these are never persisted to
    // the DB (stage props, not player-owned, same as container loot).
    struct WorldItemRecord
    {
        std::string itemId;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        int16_t stackCount = 1;
    };

    // One multiplayer instance's worth of shared world state -- everything
    // that used to live directly on EchoServer (container loot rolls, item
    // spawn rolls, ground-item pickup arbitration, door states, server-
    // driven zombie AI/ownership) now belongs to whichever Room the
    // sessions playing together are members of, so two concurrent squads
    // never see or affect each other's zombies/loot/doors.
    //
    // Part of the Login/Game server split (매칭 서버 설계): EchoServer
    // creates a fresh Room per matched squad (2~4 sessions) and destroys it
    // once it empties -- see EchoServer::EnqueueForMatch/CreateRoomForSquad
    // and Matchmaker.h for how a squad actually forms. A session doesn't
    // get a Room the instant it logs in anymore; it may sit queued for a
    // squad-mate for a short while first (see Session::GetRoom/SetRoom).
    class Room : public std::enable_shared_from_this<Room>
    {
    public:
        explicit Room(uint32_t roomId) : roomId_(roomId) {}

        uint32_t GetId() const { return roomId_; }

        /*-------------------
         멤버십
        -------------------*/
        void AddSession(std::shared_ptr<Session> session);
        void RemoveSession(uint32_t sessionId);
        size_t MemberCount() const;

        /*-------------------
         브로드캐스트 (이 Room의 멤버에게만)
        -------------------*/
        void Broadcast(uint32_t excludeSessionId, const char* data, uint32_t len);
        std::vector<std::shared_ptr<Session>> SnapshotOtherSessions(uint32_t excludeSessionId);

        /*-------------------
         월드 아이템 상태 (컨테이너 루팅 동기화)
        -------------------*/
        const std::vector<InventoryItemRecord>& ClaimContainerLoot(
            uint32_t containerId, std::vector<InventoryItemRecord> proposed);
        const std::vector<WorldItemRecord>& ClaimItemSpawnRoll(
            uint32_t spawnPointId, std::vector<WorldItemRecord> proposed);

        /*-------------------
         문 상태 (늦참 동기화)
        -------------------*/
        void SetDoorState(uint32_t doorId, bool isOpen);
        std::vector<std::pair<uint32_t, bool>> SnapshotDoorStates() const;

        // First PICKUP of a given ground item wins -- see the original
        // EchoServer::ClaimItemPickup comment (unchanged reasoning, just
        // scoped to this Room's members now).
        bool ClaimItemPickup(uint32_t netSlotId, uint32_t sessionId);

        /*-------------------
         적(좀비) AI 소유권 (레거시 클라이언트-주도 경로)
        -------------------*/
        bool ClaimEnemy(uint32_t enemyId, uint32_t sessionId);
        std::vector<uint32_t> ReleaseEnemiesOwnedBy(uint32_t sessionId);
        std::shared_ptr<Session> FindEnemyOwnerSession(uint32_t enemyId);

        /*-------------------
         서버 권위 적(좀비) AI
        -------------------*/
        void LoadEnemyObstacles(const std::string& path);
        void RegisterServerEnemy(uint32_t enemyId, const FEnemyAiRecord& initial);
        bool ApplyServerEnemyDamage(uint32_t enemyId, float damage);

        // Advances this room's enemyAi_ by deltaSeconds and broadcasts/
        // unicasts the results to this room's own members only -- see
        // EchoServer::BackgroundTickLoop for the thread/timing this is
        // called from (once per live Room, every tick).
        void Tick(float deltaSeconds);

        // Was EchoServer::ResetWorldStateIfMultiMapEmpty, room-scoped: forgets
        // every claim (loot/item spawns/pickups/doors/enemies) this room has
        // accumulated once no member is currently visible (see Session::
        // IsVisible) -- an un-expiring claim would otherwise mean this room's
        // Multi map never sees fresh content again for whoever re-enters it.
        void ResetWorldStateIfNoVisibleMembers();

    private:
        // Tells `newMember` who else is already in this room (roster +
        // their current weapon) and every door already toggled this room's
        // run, then tells the REST of the room that `newMember` just
        // joined -- was C2S_Login's own "roster reveal" (steps 2/2b/3 of
        // that case) before matchmaking made login and room-membership two
        // separate events (see EchoServer::EnqueueForMatch/Matchmaker.h):
        // a session doesn't necessarily get a Room the instant it logs in
        // anymore, so this fires from AddSession -- whenever THAT actually
        // happens -- instead.
        void AnnounceNewMember(const std::shared_ptr<Session>& newMember);

        uint32_t roomId_;

        mutable std::mutex membersLock_;
        std::unordered_map<uint32_t, std::shared_ptr<Session>> members_;

        std::mutex containerLootLock_;
        std::unordered_map<uint32_t, std::vector<InventoryItemRecord>> containerLoot_;

        std::mutex itemSpawnLock_;
        std::unordered_map<uint32_t, std::vector<WorldItemRecord>> itemSpawnRolls_;

        std::mutex itemPickupLock_;
        std::unordered_map<uint32_t, uint32_t> pickedUpItems_; // net_slot_id -> picking session id

        mutable std::mutex doorStateLock_;
        std::unordered_map<uint32_t, bool> doorStates_; // door_id -> is_open

        std::mutex enemyOwnerLock_;
        std::unordered_map<uint32_t, uint32_t> enemyOwners_; // enemy_id -> owning session id

        EnemyAI enemyAi_;
    };
}
