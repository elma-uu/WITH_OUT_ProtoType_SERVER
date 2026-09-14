#pragma once
#include "LevelObstacles.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Wop
{
    struct FEnemyAiRecord
    {
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float lookYaw = 0.0f;
        float health = 100.0f;
        float maxHealth = 100.0f;
        float moveSpeed = 300.0f;
        float attackRange = 150.0f;
        float attackDamage = 10.0f;
        float attackCooldown = 1.0f;
        // Counts down only while a player is within attackRange (see
        // EnemyAI::Tick) -- starts at 0 so the first player to walk into
        // range gets hit right away instead of waiting out a full cooldown.
        float attackCooldownRemaining = 0.0f;
        bool isDead = false;

        // 문제: "싱글과 똑같이 피격 판정을 넣어줘" -- 싱글 플레이(AEnemyBase)는
        // 공격 몽타주 재생 -> 애님 노티파이(BeginAttack)가 열릴 때만 실제
        // 타격 판정용 콜리전 박스(LeftHandAttackBox)가 활성화되는 방식이라,
        // "스윙 시작"과 "데미지 판정"이 시간차를 두고 일어나고(그 사이에
        // 플레이어가 실제로 피해서 빗나갈 수 있음), 판정도 그 순간의 실제
        // 콜리전 오버랩이다. 반면 서버(멀티)는 지금까지 attackCooldownRemaining이
        // 0이 되는 바로 그 틱에 사거리 안이면 무조건 즉시 명중 처리했다 --
        // 스윙 애니메이션이 시작되는 것과 데미지가 들어가는 게 동시라 피할
        // 시간 자체가 없었다. 아래 두 필드로 그 간극을 흉내낸다: 공격을
        // "결정"하는 순간엔 애니메이션만 브로드캐스트하고(attackStartEvents),
        // kAttackWindupSeconds만큼 실제로 기다렸다가 그 시점의 (변했을 수도
        // 있는) 위치로 다시 사거리/시야 체크를 해서 진짜로 맞았을 때만
        // 데미지를 넣는다(attackHitEvents) -- 서버엔 본/스켈레톤이 없어서
        // 손 콜리전 박스 자체를 흉내낼 순 없지만, 최소한 "스윙 도중 빠지면
        // 빗나간다"는 핵심 체감은 이걸로 재현된다.
        bool isAttackWindingUp = false;
        float attackWindupRemaining = 0.0f;
        uint32_t pendingAttackTargetSessionId = 0;

        // 문제: "A* 알고리즘 적용해 줄 수 있어?" -- LevelObstacles::FindPath가
        // 돌려준 웨이포인트를 따라가는 동안의 상태. currentPath가 비어있으면
        // "A*를 못 썼다"는 뜻이고(그리드 미구축/도달 불가/그리드 범위 밖)
        // Tick()이 기존 반응형 좌우 회피로 폴백한다 -- 새 경로탐색이 실패해도
        // 절대 제자리에 얼어붙지 않는다는 그 기존 안전장치는 그대로 유지된다.
        std::vector<std::pair<float, float>> currentPath;
        size_t currentPathIdx = 0;
        // 매 틱 A*를 다시 돌리면 낭비고(타겟이 몇 cm 움직였다고 매번
        // 재계산할 필요 없음), 0이 되면 재계산을 "허용"만 한다 -- 실제
        // 재계산은 타겟이 lastPathTargetX/Y에서 충분히 멀어졌을 때만
        // 일어난다(Tick() 참고).
        float pathRecomputeCooldownRemaining = 0.0f;
        float lastPathTargetX = 0.0f;
        float lastPathTargetY = 0.0f;

        // Caller-type only (AEnemyCaller) -- see C2S_EnemyRegister's
        // schema comment and EnemyAI::Tick's call sweep.
        bool isCaller = false;
        float callRadius = 0.0f;
        float callCooldown = 0.0f;
        float callCooldownRemaining = 0.0f;

        // 0 = no override, chase whichever player is nearest (the
        // default). Set once by a Caller's call sweep to lock this enemy
        // onto the SAME player the caller is engaged with, even if a
        // different player would otherwise be nearer -- see Tick(). Never
        // cleared once set (matches AEnemyBase::ReceiveCallTarget's own
        // "called once, sticks" behavior); falls back to nearest-player if
        // the forced target disconnects.
        uint32_t forcedTargetSessionId = 0;
    };

    struct FEnemyStateUpdate
    {
        uint32_t enemyId = 0;
        FEnemyAiRecord record;
    };

    // One player identified by session id, for EnemyAI::Tick to know WHO
    // to send an attack event to -- plain positions alone (the old
    // interface) can't identify a target.
    struct FEnemyAiPlayerSnapshot
    {
        uint32_t sessionId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // A server-driven enemy_id's attack landing on target's own client --
    // see S2C_EnemyAttackResult's schema comment. The server decides
    // if/when/how much damage lands; the target's client is trusted to
    // apply it, same tier as C2S_AttackRequest's gun damage already is.
    struct FEnemyAttackEvent
    {
        uint32_t enemyId = 0;
        uint32_t targetSessionId = 0;
        float damage = 0.0f;
    };

    // A server-driven enemy_id STARTING its swing (windup) -- everyone in
    // the room should play the attack animation now (see
    // S2C_EnemyAttackBroadcast), same as before, just no longer bundled
    // with the damage decision: that's resolved separately, later, once
    // FEnemyAiRecord::attackWindupRemaining elapses (see this struct's
    // attackHitEvents and FEnemyAiRecord::isAttackWindingUp's comment).
    struct FEnemyAttackStartEvent
    {
        uint32_t enemyId = 0;
    };

    struct FEnemyTickResult
    {
        std::vector<FEnemyStateUpdate> stateUpdates;
        // Swing just started -- broadcast the animation only (no damage
        // decided yet).
        std::vector<FEnemyAttackStartEvent> attackStartEvents;
        // Windup just elapsed AND the target was still actually in range/
        // sight at that moment -- damage really lands now. A windup that
        // elapses with the target no longer in range/sight produces no
        // entry here at all (a clean miss, same as swinging at empty air in
        // single player).
        std::vector<FEnemyAttackEvent> attackHitEvents;
    };

    // Server-side authority for every AEnemyBase that registers itself
    // while its owning client is in a multiplayer-visible map (see
    // C2S_EnemyRegister's schema comment) -- moves each one toward the
    // nearest connected player every tick, with simple obstacle-aware
    // steering (see LevelObstacles), and is the single source of truth for
    // its position/health/death from then on. Enemies never registered
    // through this path (Single map, or nobody's registered them yet)
    // don't exist here at all -- see AEnemyBase's own client-local
    // ClaimEnemy fallback for those.
    //
    // NOT real pathfinding: obstacle avoidance here is a reactive
    // left/right deflection probe, not NavMesh/A*. See this class's Tick()
    // for the actual approximation.
    class EnemyAI
    {
    public:
        // No-op (obstacles_.Count() stays 0, i.e. "nothing blocks
        // anything") if path doesn't exist -- see LevelObstacles::LoadFromFile.
        void LoadObstacles(const std::string& path);

        // Registers enemyId at the given starting state if this is the
        // first registration seen for it; a no-op otherwise (first
        // reporter's starting position/stats win).
        void RegisterIfNew(uint32_t enemyId, const FEnemyAiRecord& initial);

        // Applies damage directly -- the server IS the health authority
        // for every enemy tracked here. Returns false if enemyId isn't
        // registered (i.e. it's not server-driven -- the caller should
        // fall back to the client-ownership C2S_EnemyDamage relay instead).
        bool ApplyDamage(uint32_t enemyId, float damage);

        // Advances every tracked (non-dead) enemy by deltaSeconds, steering
        // each toward its target player -- whichever one is nearest (2D
        // distance), unless forcedTargetSessionId overrides that (see the
        // field comment). Once within attackRange, holds position and --
        // on attackCooldown -- starts a windup (FEnemyAttackStartEvent,
        // animation only) instead of moving; the actual hit/miss decision
        // (FEnemyAttackEvent) is resolved separately once the windup
        // elapses, against wherever the target actually is BY THEN -- see
        // FEnemyAiRecord::isAttackWindingUp's comment. A second pass then lets any
        // isCaller enemy that's engaged (within its own attackRange of
        // its target) and off callCooldown force every not-yet-forced
        // enemy within callRadius onto that same target, mirroring
        // AEnemyCaller::DoCall's "call nearby zombies" for the
        // server-driven path (client-local DoCall never runs for these --
        // see AEnemyBase::Tick's bIsNetworkOwner gate). Returns every
        // enemy's current state for the caller to broadcast (there's no
        // per-enemy dirty-tracking, the caller's own tick rate is the
        // throttle) alongside any attacks landed this tick.
        FEnemyTickResult Tick(float deltaSeconds, const std::vector<FEnemyAiPlayerSnapshot>& players);

        // Forgets every registered enemy (health, death, forced targets --
        // everything RegisterIfNew/ApplyDamage/Tick have accumulated), so
        // the next client to register enemy_id starts it fresh instead of
        // "already dead/damaged from an earlier session" -- see
        // EchoServer::UnregisterSession's comment for when/why this runs.
        // obstacles_ is level geometry, not per-session state, and is left
        // alone.
        void Reset();

    private:
        std::mutex lock_;
        std::unordered_map<uint32_t, FEnemyAiRecord> enemies_;
        LevelObstacles obstacles_;
    };
}
