#include "EnemyAI.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Wop
{
    namespace
    {
        constexpr float kEnemyRadius = 40.0f;     // approximate capsule radius, for obstacle inflation
        constexpr float kDeflectDegrees = 45.0f;  // how sharply to try steering around a blocked path
        constexpr float kPi = 3.14159265358979323846f;
    }

    void EnemyAI::LoadObstacles(const std::string& path)
    {
        obstacles_.LoadFromFile(path);
    }

    void EnemyAI::RegisterIfNew(uint32_t enemyId, const FEnemyAiRecord& initial)
    {
        std::lock_guard<std::mutex> guard(lock_);
        enemies_.try_emplace(enemyId, initial);
    }

    bool EnemyAI::ApplyDamage(uint32_t enemyId, float damage)
    {
        std::lock_guard<std::mutex> guard(lock_);
        const auto it = enemies_.find(enemyId);
        if (it == enemies_.end())
            return false;

        if (damage > 0.0f && !it->second.isDead)
        {
            it->second.health = std::max(0.0f, it->second.health - damage);
            if (it->second.health <= 0.0f)
            {
                it->second.isDead = true;
            }
        }
        return true;
    }

    std::vector<FEnemyStateUpdate> EnemyAI::Tick(float deltaSeconds, const std::vector<std::array<float, 3>>& playerPositions)
    {
        std::vector<FEnemyStateUpdate> updates;
        std::lock_guard<std::mutex> guard(lock_);
        updates.reserve(enemies_.size());

        for (auto& [enemyId, record] : enemies_)
        {
            if (record.isDead || playerPositions.empty())
            {
                updates.push_back({ enemyId, record });
                continue;
            }

            // Nearest player, 2D distance (height doesn't matter for chasing).
            size_t nearestIdx = 0;
            float nearestDistSq = std::numeric_limits<float>::max();
            for (size_t i = 0; i < playerPositions.size(); ++i)
            {
                const float dx = playerPositions[i][0] - record.posX;
                const float dy = playerPositions[i][1] - record.posY;
                const float distSq = dx * dx + dy * dy;
                if (distSq < nearestDistSq)
                {
                    nearestDistSq = distSq;
                    nearestIdx = i;
                }
            }

            const float nearestDist = std::sqrt(nearestDistSq);

            if (nearestDist > record.attackRange)
            {
                const float targetX = playerPositions[nearestIdx][0];
                const float targetY = playerPositions[nearestIdx][1];

                float dirX = (targetX - record.posX) / std::max(nearestDist, 0.0001f);
                float dirY = (targetY - record.posY) / std::max(nearestDist, 0.0001f);

                const float moveDist = record.moveSpeed * deltaSeconds;
                const float probeDist = moveDist + kEnemyRadius;

                if (obstacles_.SegmentBlocked(record.posX, record.posY,
                        record.posX + dirX * probeDist, record.posY + dirY * probeDist, kEnemyRadius))
                {
                    // Direct path blocked -- try deflecting left/right
                    // around whatever's in the way. Reactive, not real
                    // pathfinding: see this class's header comment.
                    const float rad = kDeflectDegrees * kPi / 180.0f;
                    const float cosA = std::cos(rad);
                    const float sinA = std::sin(rad);
                    const float leftX = dirX * cosA - dirY * sinA;
                    const float leftY = dirX * sinA + dirY * cosA;
                    const float rightX = dirX * cosA + dirY * sinA;
                    const float rightY = -dirX * sinA + dirY * cosA;

                    const bool leftBlocked = obstacles_.SegmentBlocked(record.posX, record.posY,
                        record.posX + leftX * probeDist, record.posY + leftY * probeDist, kEnemyRadius);
                    const bool rightBlocked = obstacles_.SegmentBlocked(record.posX, record.posY,
                        record.posX + rightX * probeDist, record.posY + rightY * probeDist, kEnemyRadius);

                    if (!leftBlocked)
                    {
                        dirX = leftX;
                        dirY = leftY;
                    }
                    else if (!rightBlocked)
                    {
                        dirX = rightX;
                        dirY = rightY;
                    }
                    // else: both deflections also blocked -- keep going
                    // straight anyway. Better than freezing in place; a
                    // real wall-follow algorithm would do better than this.
                }

                record.posX += dirX * moveDist;
                record.posY += dirY * moveDist;
                record.lookYaw = std::atan2(dirY, dirX) * 180.0f / kPi;
            }
            // else: already within attack range -- hold position. Attack
            // animation/damage-to-player isn't wired up for the
            // server-driven path yet (see EnemyBase.cpp's comment on
            // HandleEnemyState) -- movement only, for now.

            updates.push_back({ enemyId, record });
        }

        return updates;
    }
}
