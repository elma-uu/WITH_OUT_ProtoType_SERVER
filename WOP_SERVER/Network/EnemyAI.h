#pragma once
#include "LevelObstacles.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
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

    struct FEnemyTickResult
    {
        std::vector<FEnemyStateUpdate> stateUpdates;
        std::vector<FEnemyAttackEvent> attackEvents;
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
        // on attackCooldown -- emits an FEnemyAttackEvent against its
        // target instead of moving. A second pass then lets any
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
