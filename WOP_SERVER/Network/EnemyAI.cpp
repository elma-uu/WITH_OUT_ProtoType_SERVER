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

    FEnemyTickResult EnemyAI::Tick(float deltaSeconds, const std::vector<FEnemyAiPlayerSnapshot>& players)
    {
        FEnemyTickResult result;
        std::lock_guard<std::mutex> guard(lock_);
        result.stateUpdates.reserve(enemies_.size());

        // Resolves which player (index into `players`) a record should
        // chase: its forced target if one is set and still connected (see
        // FEnemyAiRecord::forcedTargetSessionId), else whichever is
        // nearest (2D distance). Only called with a non-empty `players`.
        auto resolveTargetIdx = [&](const FEnemyAiRecord& rec) -> size_t
        {
            if (rec.forcedTargetSessionId != 0)
            {
                for (size_t i = 0; i < players.size(); ++i)
                {
                    if (players[i].sessionId == rec.forcedTargetSessionId)
                        return i;
                }
                // Forced target disconnected -- fall through to nearest.
            }

            size_t nearestIdx = 0;
            float nearestDistSq = std::numeric_limits<float>::max();
            for (size_t i = 0; i < players.size(); ++i)
            {
                const float dx = players[i].x - rec.posX;
                const float dy = players[i].y - rec.posY;
                const float distSq = dx * dx + dy * dy;
                if (distSq < nearestDistSq)
                {
                    nearestDistSq = distSq;
                    nearestIdx = i;
                }
            }
            return nearestIdx;
        };

        for (auto& [enemyId, record] : enemies_)
        {
            if (record.isDead || players.empty())
            {
                result.stateUpdates.push_back({ enemyId, record });
                continue;
            }

            const size_t targetIdx = resolveTargetIdx(record);
            const float targetDx = players[targetIdx].x - record.posX;
            const float targetDy = players[targetIdx].y - record.posY;
            const float nearestDist = std::sqrt(targetDx * targetDx + targetDy * targetDy);

            if (nearestDist > record.attackRange)
            {
                // Out of range -- chasing, so the attack timer doesn't
                // advance (see FEnemyAiRecord::attackCooldownRemaining):
                // walking away and back doesn't buy an instant free hit,
                // but it doesn't get punished either, whatever was left
                // just resumes.
                const float targetX = players[targetIdx].x;
                const float targetY = players[targetIdx].y;

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
            else
            {
                // Within attack range -- hold position and swing on
                // cooldown. No telegraph/animation sync yet (clients just
                // see the health tick down); see this record's field
                // comment for why the timer starts at 0 (hits on arrival).
                record.attackCooldownRemaining -= deltaSeconds;
                if (record.attackCooldownRemaining <= 0.0f)
                {
                    record.attackCooldownRemaining = record.attackCooldown;
                    result.attackEvents.push_back({ enemyId, players[targetIdx].sessionId, record.attackDamage });
                }

                // Caller-type, engaged with its own target -- periodically
                // forces nearby not-yet-forced enemies onto that same
                // target (mirrors AEnemyCaller::DoCall; see this class's
                // header comment for why the client-local version never
                // runs for a server-driven enemy).
                if (record.isCaller)
                {
                    record.callCooldownRemaining -= deltaSeconds;
                    if (record.callCooldownRemaining <= 0.0f)
                    {
                        record.callCooldownRemaining = record.callCooldown;
                        const uint32_t forcedSessionId = players[targetIdx].sessionId;

                        for (auto& [otherId, otherRecord] : enemies_)
                        {
                            if (otherId == enemyId || otherRecord.isDead || otherRecord.forcedTargetSessionId != 0)
                                continue;

                            const float cdx = otherRecord.posX - record.posX;
                            const float cdy = otherRecord.posY - record.posY;
                            if (cdx * cdx + cdy * cdy <= record.callRadius * record.callRadius)
                            {
                                otherRecord.forcedTargetSessionId = forcedSessionId;
                            }
                        }
                    }
                }
            }

            result.stateUpdates.push_back({ enemyId, record });
        }

        return result;
    }
}
