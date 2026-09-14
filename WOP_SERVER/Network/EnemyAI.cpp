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

        // 싱글 플레이(AEnemyBase)의 공격 몽타주에서 "BeginAttack" 애님
        // 노티파이가 스윙 시작 후 대략 이 정도 지나서 열린다 -- 서버엔
        // 애니메이션이 없어 몽타주 타이밍을 그대로 읽을 순 없으니, 전형적인
        // 좀비 근접 공격 예비동작(호밍/휘두르는 느낌) 길이로 근사한
        // 값이다. 이 시간 동안은 애니메이션만 브로드캐스트되고 데미지
        // 판정은 보류된다(FEnemyAiRecord::isAttackWindingUp 참고).
        constexpr float kAttackWindupSeconds = 0.4f;
    }

    void EnemyAI::LoadObstacles(const std::string& path)
    {
        obstacles_.LoadFromFile(path);
    }

    void EnemyAI::RegisterIfNew(uint32_t enemyId, const FEnemyAiRecord& initial)
    {
        std::lock_guard<std::mutex> guard(lock_);
        const auto [it, inserted] = enemies_.try_emplace(enemyId, initial);
        // 진단 로그(문제: "이따금씩 유령 좀비 -- 투명한 좀비에게 공격당함") --
        // 같은 enemy_id를 두 번째 이상 등록하려는 시도(inserted==false)는
        // 정상적으로 무시되지만(첫 등록자가 권위를 가짐), 그 두 번째
        // 클라이언트 쪽 로컬 액터가 미러 모드로 제대로 전환됐는지는 서버가
        // 알 수 없다 -- 이 로그로 "몇 번이나, 누가 재등록을 시도했는지"부터
        // 확인한다.
        std::printf("[EnemyAI] RegisterIfNew(enemy_id=%u): %s (pos=%.0f,%.0f,%.0f)\n",
                    enemyId, inserted ? "accepted (first registration)" : "ignored (already registered)",
                    initial.posX, initial.posY, initial.posZ);
    }

    void EnemyAI::Reset()
    {
        std::lock_guard<std::mutex> guard(lock_);
        enemies_.clear();
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

            // 스윙 예비동작 중 -- 싱글 플레이의 "몽타주 재생 중이면 제자리에
            // 멈춰서 애님 노티파이가 열릴 때까지 기다린다"와 동일하게, 이번
            // 틱은 새 타겟 재탐색/이동/재공격 판단을 전부 건너뛰고 예비동작
            // 타이머만 줄인다.
            if (record.isAttackWindingUp)
            {
                record.attackWindupRemaining -= deltaSeconds;
                if (record.attackWindupRemaining <= 0.0f)
                {
                    record.isAttackWindingUp = false;

                    // 예비동작이 걸린 "그 순간"이 아니라 지금(=명중 판정
                    // 시점)의 타겟 위치로 다시 사거리/시야를 검사한다 --
                    // 싱글의 손 콜리전 박스가 스윙 도중 실제로 거기 있어야
                    // 맞는 것과 같은 이치. 타겟이 그새 사거리를 벗어났거나
                    // (도망), 시야가 막혔거나(장애물 뒤로 숨음), 아예
                    // 연결이 끊겼으면 그냥 빗나간다 -- 이벤트 자체를 만들지
                    // 않는다.
                    for (size_t i = 0; i < players.size(); ++i)
                    {
                        if (players[i].sessionId != record.pendingAttackTargetSessionId)
                            continue;

                        const float hitDx = players[i].x - record.posX;
                        const float hitDy = players[i].y - record.posY;
                        const float hitDist = std::sqrt(hitDx * hitDx + hitDy * hitDy);
                        const bool stillHasLineOfSight = !obstacles_.SegmentBlocked(
                            record.posX, record.posY, players[i].x, players[i].y, 0.0f);

                        if (hitDist <= record.attackRange && stillHasLineOfSight)
                        {
                            result.attackHitEvents.push_back(
                                { enemyId, players[i].sessionId, record.attackDamage });
                        }
                        break;
                    }

                    record.pendingAttackTargetSessionId = 0;
                }

                result.stateUpdates.push_back({ enemyId, record });
                continue;
            }

            const size_t targetIdx = resolveTargetIdx(record);
            const float targetDx = players[targetIdx].x - record.posX;
            const float targetDy = players[targetIdx].y - record.posY;
            const float nearestDist = std::sqrt(targetDx * targetDx + targetDy * targetDy);

            // attackRange alone is pure 2D Euclidean distance -- it doesn't
            // know a wall is in the way, so a zombie standing right against
            // one side of a wall could keep landing hits on a player just
            // as close on the OTHER side (문제: "벽 너머의 플레이어를
            // 공격해"). obstacles_ already exists for movement steering
            // below; reuse it here as a straight-line-of-sight test (radius
            // 0 -- a thin sightline, not the inflated movement corridor
            // check further down) so a LOS-blocked target counts as
            // out-of-range: this enemy keeps trying to path around the wall
            // (same steering as any other out-of-range case) instead of
            // freezing in a spot where it can see nothing but a wall and
            // hit a player it can't actually reach.
            const bool hasLineOfSight = !obstacles_.SegmentBlocked(
                record.posX, record.posY, players[targetIdx].x, players[targetIdx].y, 0.0f);

            if (nearestDist > record.attackRange || !hasLineOfSight)
            {
                // Out of range -- chasing, so the attack timer doesn't
                // advance (see FEnemyAiRecord::attackCooldownRemaining):
                // walking away and back doesn't buy an instant free hit,
                // but it doesn't get punished either, whatever was left
                // just resumes.
                const float targetX = players[targetIdx].x;
                const float targetY = players[targetIdx].y;
                const float moveDist = record.moveSpeed * deltaSeconds;

                // 문제: "A* 알고리즘 적용해 줄 수 있어?" -- 경로가 아직
                // 없거나(pathExhausted) 다 따라갔거나, 타겟이 마지막으로
                // 경로를 계산했던 지점에서 충분히 멀어졌으면 재계산한다.
                // pathRecomputeCooldownRemaining은 그 재계산 자체를 매 틱
                // 새로 돌리는 낭비(+타겟이 경계값 근처에서 왔다갔다 할 때의
                // 스래싱)를 막는 하한선일 뿐 -- 경로를 다 따라간 경우엔
                // 쿨다운이 남아있어도 이번 틱은 그냥 반응형 폴백으로
                // 움직이고, 쿨다운이 풀리는 다음 틱에 바로 재계산된다.
                record.pathRecomputeCooldownRemaining -= deltaSeconds;

                constexpr float kRepathTargetMoveThreshold = 300.0f;
                const float targetMoveDx = targetX - record.lastPathTargetX;
                const float targetMoveDy = targetY - record.lastPathTargetY;
                const bool targetMovedEnough =
                    (targetMoveDx * targetMoveDx + targetMoveDy * targetMoveDy)
                    > (kRepathTargetMoveThreshold * kRepathTargetMoveThreshold);
                const bool pathExhausted =
                    record.currentPath.empty() || record.currentPathIdx >= record.currentPath.size();

                if (record.pathRecomputeCooldownRemaining <= 0.0f && (pathExhausted || targetMovedEnough))
                {
                    std::vector<std::pair<float, float>> newPath;
                    if (obstacles_.FindPath(record.posX, record.posY, targetX, targetY, kEnemyRadius, newPath))
                    {
                        record.currentPath = std::move(newPath);
                        record.currentPathIdx = 0;
                    }
                    else
                    {
                        // 도달 불가/그리드 미구축/그리드 범위 밖 -- 아래
                        // 반응형 좌우 회피 폴백으로 넘어간다(기존 동작
                        // 그대로, 절대 제자리에 얼어붙지 않음).
                        record.currentPath.clear();
                    }
                    record.pathRecomputeCooldownRemaining = 0.3f;
                    record.lastPathTargetX = targetX;
                    record.lastPathTargetY = targetY;
                }

                float dirX;
                float dirY;

                if (!record.currentPath.empty() && record.currentPathIdx < record.currentPath.size())
                {
                    // A* 웨이포인트를 따라간다 -- 연속된 두 웨이포인트 사이는
                    // LevelObstacles::SmoothPath가 이미 "직선으로 걸어도
                    // 안전하다"고 검증해뒀으므로 여기선 장애물 프로브 없이
                    // 그냥 직진한다.
                    constexpr float kWaypointArrivalRadius = 60.0f;
                    float wx = record.currentPath[record.currentPathIdx].first;
                    float wy = record.currentPath[record.currentPathIdx].second;
                    float toWpX = wx - record.posX;
                    float toWpY = wy - record.posY;
                    float wpDist = std::sqrt(toWpX * toWpX + toWpY * toWpY);

                    while (wpDist <= kWaypointArrivalRadius &&
                           record.currentPathIdx + 1 < record.currentPath.size())
                    {
                        ++record.currentPathIdx;
                        wx = record.currentPath[record.currentPathIdx].first;
                        wy = record.currentPath[record.currentPathIdx].second;
                        toWpX = wx - record.posX;
                        toWpY = wy - record.posY;
                        wpDist = std::sqrt(toWpX * toWpX + toWpY * toWpY);
                    }

                    // 버그였던 부분(문제: "좀비가 아예 멈춰버리는데") -- 위
                    // while 루프는 "다음" 웨이포인트가 있을 때만
                    // currentPathIdx를 증가시키니까, 마지막 웨이포인트에
                    // 도착한 뒤로는 currentPathIdx가 (size-1)에서 영원히
                    // 멈춰있었다. pathExhausted 판정은 currentPathIdx >=
                    // size()만 보는데 그 조건이 절대 참이 될 수 없었으니,
                    // 목표가 lastPathTarget에서 300유닛 이상 움직이기
                    // 전까지는 재계산도 안 일어나고 -- 이미 다 도착한
                    // 마지막 지점 근처에서 거의 0에 가까운 발걸음만 계속
                    // 내딛는(=사실상 정지) 상태로 갇혔다. 마지막 웨이포인트에
                    // 실제로 도착했으면 currentPathIdx를 size()까지 밀어서
                    // "경로 다 씀"을 명시적으로 표시한다 -- 다음 틱에
                    // pathExhausted가 바로 true가 되어 그 순간의 진짜 목표
                    // 위치로 즉시 재탐색한다.
                    if (wpDist <= kWaypointArrivalRadius)
                    {
                        record.currentPathIdx = record.currentPath.size();
                    }

                    dirX = toWpX / std::max(wpDist, 0.0001f);
                    dirY = toWpY / std::max(wpDist, 0.0001f);
                }
                else
                {
                    // 반응형 좌우 회피 폴백(원래 유일했던 스티어링) -- A*가
                    // 이 타겟에 대해 실패했을 때만 탄다.
                    dirX = (targetX - record.posX) / std::max(nearestDist, 0.0001f);
                    dirY = (targetY - record.posY) / std::max(nearestDist, 0.0001f);

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
                }

                record.posX += dirX * moveDist;
                record.posY += dirY * moveDist;
                record.lookYaw = std::atan2(dirY, dirX) * 180.0f / kPi;
            }
            else
            {
                // Within attack range -- hold position and swing on
                // cooldown. 싱글과 똑같이 피격 판정을 넣기 위해, 여기선 더
                // 이상 즉시 명중시키지 않는다 -- 애니메이션 브로드캐스트만
                // 지금 내보내고(attackStartEvents), 실제 명중/빗나감 판단은
                // kAttackWindupSeconds 뒤 이 record.isAttackWindingUp 분기
                // (이 함수 맨 위)에서 그 시점의 실제 위치로 다시 내린다 --
                // 이 record.attackCooldownRemaining이 0인 이 틱은 스윙 판단만
                // 내렸을 뿐이라는 점에서 여전히 "hits on arrival"이지만,
                // 그게 곧 데미지 확정은 아니게 됐다.
                record.attackCooldownRemaining -= deltaSeconds;
                if (record.attackCooldownRemaining <= 0.0f)
                {
                    record.attackCooldownRemaining = record.attackCooldown;
                    record.isAttackWindingUp = true;
                    record.attackWindupRemaining = kAttackWindupSeconds;
                    record.pendingAttackTargetSessionId = players[targetIdx].sessionId;
                    result.attackStartEvents.push_back({ enemyId });
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
