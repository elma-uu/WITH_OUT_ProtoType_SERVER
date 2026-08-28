#pragma once
#include "LevelObstacles.h"
#include <array>
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
        bool isDead = false;
    };

    struct FEnemyStateUpdate
    {
        uint32_t enemyId = 0;
        FEnemyAiRecord record;
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
        // each toward whichever position in playerPositions is nearest (2D
        // distance). Returns every enemy's current state for the caller to
        // broadcast -- there's no per-enemy dirty-tracking, the caller's
        // own tick rate is the throttle.
        std::vector<FEnemyStateUpdate> Tick(float deltaSeconds, const std::vector<std::array<float, 3>>& playerPositions);

    private:
        std::mutex lock_;
        std::unordered_map<uint32_t, FEnemyAiRecord> enemies_;
        LevelObstacles obstacles_;
    };
}
