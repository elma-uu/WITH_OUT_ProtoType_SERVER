#pragma once
#include "Database.h"
#include "EnemyAI.h"
#include <chrono>
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
        // resyncInterval/resyncWindow default to the real production values
        // (MaybeResyncAllMembers's comment) -- overridable so the live
        // socket test suite can set an effectively-infinite interval
        // (WOP_RESYNC_INTERVAL_MS, see ServerMain.cpp) instead of getting
        // extra S2C_SendPlayerInfo/S2C_ItemUseBroadcast traffic injected
        // mid-test every second, which broke every test still doing
        // positional (not type-filtered) packet reads.
        explicit Room(uint32_t roomId,
                      std::chrono::milliseconds resyncInterval = kDefaultResyncInterval,
                      std::chrono::milliseconds resyncWindow = kDefaultResyncWindow)
            // 선언 순서(resyncInterval_/resyncWindow_가 roomId_보다 먼저 선언됨)와
            // 맞춰서 나열 -- 실제 초기화는 어차피 선언 순서대로 일어나지만
            // 경고 방지 + 가독성을 위해.
            : resyncInterval_(resyncInterval)
            , resyncWindow_(resyncWindow)
            , roomId_(roomId)
        {
        }

        static constexpr std::chrono::milliseconds kDefaultResyncInterval{1000};
        static constexpr std::chrono::milliseconds kDefaultResyncWindow{15000};

        uint32_t GetId() const { return roomId_; }

        /*-------------------
         멤버십
        -------------------*/
        void AddSession(std::shared_ptr<Session> session);
        void RemoveSession(uint32_t sessionId);
        size_t MemberCount() const;

        // 문제: "먼저 들어온 사람 화면에서 늦게 들어온 유저가 안 보임" --
        // C2S_MultiMapReady 핸들러(Session.cpp)에서 호출한다. AddSession이
        // 트리거하는 원래 로스터 알림은 이 세션이 Room에 추가되는 그 순간(아직
        // 레벨 로딩 중일 수도 있음) 딱 한 번만 나가는데, 그 타이밍에 뭔가
        // 놓쳤어도 복구할 방법이 없었다. 이건 클라이언트가 "나 진짜 멀티맵
        // 들어왔고 준비됐다"고 알려올 때마다 AnnounceNewMember와 완전히 동일한
        // 로직(이 세션에게 다른 멤버 전원 재통지 + 다른 멤버 전원에게 이
        // 세션 재통지)을 다시 실행해서 자체 복구한다 -- AddSession 때 이미
        // 멤버였던 세션이라도 상관없이 그냥 다시 알려주는 것뿐이라 안전하다.
        void ReannounceMember(const std::shared_ptr<Session>& session);

        // 문제: "먼저 들어온 사람 화면에서 늦게 들어온 유저가 안 보임"이
        // C2S_MultiMapReady로도 실기에서 100% 재현/특정이 안 되는 상황(친구
        // PC라 로그 확인이 어려움) -- 정확한 근본 원인(클라이언트 레벨 로딩
        // 타이밍 등)을 지금 당장 못 잡아도, 이 방이 갓 형성된 동안(kResyncWindow)
        // 주기적으로(kResyncInterval마다) 전원에게 서로의 로스터를 다시
        // 뿌려서 스스로 복구되게 한다 -- ReannounceMember 하나하나가 이미
        // 멱등적(UpdateRemotePlayer가 이미 스폰된 상대면 그냥 갱신)이라
        // 여러 번 반복해도 안전하다. Tick()에서 매 틱 호출한다(내부적으로
        // 실제 재전송은 kResyncInterval 간격으로만 실행).
        void MaybeResyncAllMembers();

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

        // See MaybeResyncAllMembers's comment. createdAt_ is set once, at
        // construction; lastResyncAt_ starts equal to it so the very first
        // Tick() after formation doesn't immediately fire an extra resync on
        // top of AddSession's own already-fresh announcements.
        const std::chrono::milliseconds resyncInterval_;
        const std::chrono::milliseconds resyncWindow_;
        const std::chrono::steady_clock::time_point createdAt_ = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastResyncAt_ = createdAt_;

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
