#include "LevelObstacles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <queue>
#include <sstream>

namespace Wop
{
    namespace
    {
        // EnemyAI.cpp의 kEnemyRadius(좀비 캡슐 반지름 근사치)와 동일한 값 --
        // 그리드를 지금 이 하나의 값으로 미리 구워두는 대신 쿼리마다 다시
        // 굽지 않기 위해 여기서도 같은 상수를 쓴다(두 파일이 서로 include
        // 관계가 아니라 공유 헤더로 뽑기보단 그냥 주석으로 동기화 명시).
        constexpr float kAgentRadius = 40.0f;

        // 그리드 전체 크기에 대한 안전장치 -- 장애물 파일이 예상보다 훨씬
        // 넓은 영역에 퍼져있어도(예: 내보내기 필터링 누락) 셀 개수가
        // 터무니없이 커지지 않게 cellSize를 필요한 만큼 늘린다.
        constexpr int kMaxGridCells = 2'000'000;

        // A* 탐색 자체의 안전장치 -- 그리드가 아무리 커도 한 번의 경로
        // 탐색이 확장하는 노드 수를 이 이하로 강제한다(도달 불가능한
        // 목표를 향해 그리드 전체를 훑는 최악의 경우 대비).
        constexpr int kMaxAStarExpansions = 60000;
    }

    bool LevelObstacles::LoadFromFile(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            std::printf("LevelObstacles: no obstacle file at '%s' -- enemies will move in straight lines only\n", path.c_str());
            return false;
        }

        obstacles_.clear();
        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream iss(line);
            FAabb2D box;
            if (iss >> box.minX >> box.minY >> box.maxX >> box.maxY)
            {
                obstacles_.push_back(box);
            }
        }

        std::printf("LevelObstacles: loaded %zu obstacle(s) from '%s'\n", obstacles_.size(), path.c_str());

        if (!obstacles_.empty())
            BuildGrid();

        return !obstacles_.empty();
    }

    void LevelObstacles::BuildGrid()
    {
        // 문제: "A* 알고리즘 적용해 줄 수 있어?" -- 서버엔 진짜 NavMesh가
        // 없으니(LevelObstacles 헤더 주석 참고), SegmentBlocked가 쓰는 것과
        // 똑같은 장애물 AABB들을 거친 격자에 래스터화해서 그 위에서 A*를
        // 돌린다. 폴리곤 기반 진짜 NavMesh보다 정밀도는 떨어지지만, 반응형
        // 좌우 회피(EnemyAI::Tick의 기존 폴백)보다는 훨씬 목적지까지 실제로
        // 돌아가는 경로를 찾는다.
        float minX = obstacles_.front().minX, minY = obstacles_.front().minY;
        float maxX = obstacles_.front().maxX, maxY = obstacles_.front().maxY;
        for (const FAabb2D& box : obstacles_)
        {
            minX = std::min(minX, box.minX);
            minY = std::min(minY, box.minY);
            maxX = std::max(maxX, box.maxX);
            maxY = std::max(maxY, box.maxY);
        }

        // 장애물들의 바운딩 박스만으로 그리드를 잡으면 그 가장자리에 딱
        // 붙은 실제 플레이 공간(장애물 자체는 없지만 그 바로 옆)이 그리드
        // 밖으로 밀려날 수 있다 -- 여유 마진을 둔다.
        constexpr float kMarginUnits = 3000.0f;
        minX -= kMarginUnits; minY -= kMarginUnits;
        maxX += kMarginUnits; maxY += kMarginUnits;

        cellSize_ = 150.0f;
        auto computeDims = [&]()
        {
            gridW_ = std::max(1, static_cast<int>((maxX - minX) / cellSize_) + 1);
            gridH_ = std::max(1, static_cast<int>((maxY - minY) / cellSize_) + 1);
        };
        computeDims();
        while (static_cast<int64_t>(gridW_) * static_cast<int64_t>(gridH_) > kMaxGridCells)
        {
            cellSize_ *= 1.5f;
            computeDims();
        }

        gridMinX_ = minX;
        gridMinY_ = minY;

        grid_.assign(static_cast<size_t>(gridW_) * static_cast<size_t>(gridH_), 0);

        // 셀 하나하나마다 전체 장애물 목록을 훑는 대신(격자 크기 x 장애물
        // 개수), 장애물 하나마다 그게 덮는 셀 범위만 훑는다(장애물 개수 x
        // 그 장애물이 덮는 평균 셀 수) -- 실제 장애물은 그리드 전체보다
        // 훨씬 작으니 이쪽이 훨씬 빠르다.
        for (const FAabb2D& box : obstacles_)
        {
            const float infMinX = box.minX - kAgentRadius;
            const float infMinY = box.minY - kAgentRadius;
            const float infMaxX = box.maxX + kAgentRadius;
            const float infMaxY = box.maxY + kAgentRadius;

            const int cx0 = std::max(0, static_cast<int>((infMinX - gridMinX_) / cellSize_));
            const int cy0 = std::max(0, static_cast<int>((infMinY - gridMinY_) / cellSize_));
            const int cx1 = std::min(gridW_ - 1, static_cast<int>((infMaxX - gridMinX_) / cellSize_));
            const int cy1 = std::min(gridH_ - 1, static_cast<int>((infMaxY - gridMinY_) / cellSize_));

            for (int cy = cy0; cy <= cy1; ++cy)
            {
                uint8_t* row = &grid_[static_cast<size_t>(cy) * gridW_];
                for (int cx = cx0; cx <= cx1; ++cx)
                    row[cx] = 1;
            }
        }

        std::printf("LevelObstacles: built %dx%d A* grid (cell=%.0fcm, bounds=[%.0f,%.0f]-[%.0f,%.0f])\n",
                    gridW_, gridH_, cellSize_, gridMinX_, gridMinY_, maxX, maxY);
    }

    namespace
    {
        // Standard slab-method segment-vs-AABB test in 2D. box is already
        // expected to be pre-inflated by the caller.
        bool SegmentIntersectsBox(float x0, float y0, float x1, float y1, const FAabb2D& box)
        {
            float tmin = 0.0f;
            float tmax = 1.0f;
            const float dx = x1 - x0;
            const float dy = y1 - y0;

            if (std::fabs(dx) < 1e-6f)
            {
                if (x0 < box.minX || x0 > box.maxX)
                    return false;
            }
            else
            {
                float t1 = (box.minX - x0) / dx;
                float t2 = (box.maxX - x0) / dx;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }

            if (std::fabs(dy) < 1e-6f)
            {
                if (y0 < box.minY || y0 > box.maxY)
                    return false;
            }
            else
            {
                float t1 = (box.minY - y0) / dy;
                float t2 = (box.maxY - y0) / dy;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }

            return true;
        }
    }

    bool LevelObstacles::SegmentBlocked(float fromX, float fromY, float toX, float toY, float radius) const
    {
        for (const FAabb2D& box : obstacles_)
        {
            const FAabb2D inflated{ box.minX - radius, box.minY - radius, box.maxX + radius, box.maxY + radius };
            if (SegmentIntersectsBox(fromX, fromY, toX, toY, inflated))
                return true;
        }
        return false;
    }

    namespace
    {
        struct FAStarNode
        {
            int cellIndex = 0;
            float fScore = 0.0f;
            // std::priority_queue is a max-heap; A* wants the lowest fScore
            // first, so this comparator is inverted.
            bool operator<(const FAStarNode& other) const { return fScore > other.fScore; }
        };
    }

    bool LevelObstacles::FindPath(float fromX, float fromY, float toX, float toY, float agentRadius,
                                   std::vector<std::pair<float, float>>& outWaypoints) const
    {
        if (gridW_ <= 0 || gridH_ <= 0)
            return false; // BuildGrid never ran -- no obstacle file loaded.

        const auto toCell = [&](float x, float y, int& cx, int& cy) -> bool
        {
            cx = static_cast<int>((x - gridMinX_) / cellSize_);
            cy = static_cast<int>((y - gridMinY_) / cellSize_);
            return cx >= 0 && cx < gridW_ && cy >= 0 && cy < gridH_;
        };

        int startCx, startCy, goalCx, goalCy;
        if (!toCell(fromX, fromY, startCx, startCy) || !toCell(toX, toY, goalCx, goalCy))
            return false; // 그리드 바깥 -- 호출자가 반응형 스티어링으로 폴백.

        const auto cellBlocked = [&](int cx, int cy) { return grid_[static_cast<size_t>(cy) * gridW_ + cx] != 0; };
        const auto cellIndex = [&](int cx, int cy) { return cy * gridW_ + cx; };

        // 시작/목표 칸 자체가 막혀있으면(에이전트 반지름만큼 부풀린 벽 바로
        // 안쪽에 서있는 경우 등) A*가 애초에 성립하지 않는다 -- 호출자
        // 폴백으로 넘긴다. agentRadius는 지금 그리드가 이미
        // kAgentRadius(=BuildGrid)로 구워져 있어서 쿼리별로 다시 반영할 수
        // 없다 -- 매개변수로 받되 그리드 굽기 때와 다르면 그냥 무시(주석
        // 참고, LevelObstacles.h).
        (void)agentRadius;
        if (cellBlocked(startCx, startCy) || cellBlocked(goalCx, goalCy))
            return false;

        if (startCx == goalCx && startCy == goalCy)
        {
            outWaypoints.clear();
            outWaypoints.emplace_back(toX, toY);
            return true;
        }

        const int totalCells = gridW_ * gridH_;
        std::vector<float> gScore(totalCells, -1.0f); // -1 = never visited
        std::vector<int32_t> cameFrom(totalCells, -1);
        std::priority_queue<FAStarNode> openSet;

        const int startIdx = cellIndex(startCx, startCy);
        const int goalIdx = cellIndex(goalCx, goalCy);
        gScore[startIdx] = 0.0f;

        const auto octileHeuristic = [&](int cx, int cy)
        {
            const int dx = std::abs(cx - goalCx);
            const int dy = std::abs(cy - goalCy);
            const int diag = std::min(dx, dy);
            const int straight = std::max(dx, dy) - diag;
            return (straight + diag * 1.41421356f) * cellSize_;
        };

        openSet.push({ startIdx, octileHeuristic(startCx, startCy) });

        static constexpr int kNeighborDx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        static constexpr int kNeighborDy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

        bool found = false;
        int expansions = 0;
        while (!openSet.empty() && expansions < kMaxAStarExpansions)
        {
            const FAStarNode current = openSet.top();
            openSet.pop();
            ++expansions;

            if (current.cellIndex == goalIdx)
            {
                found = true;
                break;
            }

            const int cx = current.cellIndex % gridW_;
            const int cy = current.cellIndex / gridW_;

            // Already found a strictly better way here since this entry was
            // queued -- std::priority_queue has no decrease-key, so stale
            // entries are just skipped instead.
            const float bestKnownG = gScore[current.cellIndex];
            const float thisNodeG = current.fScore - octileHeuristic(cx, cy);
            if (thisNodeG > bestKnownG + 0.01f)
                continue;

            for (int n = 0; n < 8; ++n)
            {
                const int nx = cx + kNeighborDx[n];
                const int ny = cy + kNeighborDy[n];
                if (nx < 0 || nx >= gridW_ || ny < 0 || ny >= gridH_)
                    continue;
                if (cellBlocked(nx, ny))
                    continue;

                // 대각선 이동이 두 맞닿은 직교 칸을 "모서리로 베어" 지나가는
                // 것(양쪽 다 막혀있으면 그 사이로 대각선 통과)을 막는다 --
                // 안 그러면 실제로는 못 지나가는 모서리를 통과하는 경로가
                // 나올 수 있다.
                if (kNeighborDx[n] != 0 && kNeighborDy[n] != 0)
                {
                    if (cellBlocked(cx + kNeighborDx[n], cy) || cellBlocked(cx, cy + kNeighborDy[n]))
                        continue;
                }

                const float moveCost = (kNeighborDx[n] != 0 && kNeighborDy[n] != 0) ? cellSize_ * 1.41421356f : cellSize_;
                const float tentativeG = bestKnownG + moveCost;
                const int neighborIdx = cellIndex(nx, ny);

                if (gScore[neighborIdx] < 0.0f || tentativeG < gScore[neighborIdx] - 0.01f)
                {
                    gScore[neighborIdx] = tentativeG;
                    cameFrom[neighborIdx] = current.cellIndex;
                    openSet.push({ neighborIdx, tentativeG + octileHeuristic(nx, ny) });
                }
            }
        }

        if (!found)
            return false; // 도달 불가(막힌 구역으로 둘러싸임) 또는 탐색 한도 초과 -- 폴백.

        std::vector<std::pair<float, float>> rawPath;
        for (int idx = goalIdx; idx != -1; idx = cameFrom[idx])
        {
            const int cx = idx % gridW_;
            const int cy = idx / gridW_;
            rawPath.emplace_back(gridMinX_ + (cx + 0.5f) * cellSize_, gridMinY_ + (cy + 0.5f) * cellSize_);
        }
        std::reverse(rawPath.begin(), rawPath.end());

        // 실제 시작/목표 좌표로 양 끝을 교체 -- 셀 중심이 아니라 요청받은
        // 정확한 좌표로 끝나야 목표 지점에 딱 붙는다.
        rawPath.front() = { fromX, fromY };
        rawPath.back() = { toX, toY };

        SmoothPath(rawPath);
        outWaypoints = std::move(rawPath);
        return true;
    }

    void LevelObstacles::SmoothPath(std::vector<std::pair<float, float>>& path) const
    {
        if (path.size() <= 2)
            return;

        std::vector<std::pair<float, float>> smoothed;
        smoothed.push_back(path.front());

        size_t anchor = 0;
        while (anchor < path.size() - 1)
        {
            size_t farthest = anchor + 1;
            for (size_t candidate = anchor + 2; candidate < path.size(); ++candidate)
            {
                const auto& [ax, ay] = path[anchor];
                const auto& [bx, by] = path[candidate];
                if (!SegmentBlocked(ax, ay, bx, by, kAgentRadius))
                    farthest = candidate;
                // 막혔다고 바로 멈추지 않고 끝까지 훑는다 -- 더 먼 지점은
                // 뚫려있는데 중간 지점만 우연히 막힌 것처럼 보이는(그리드
                // 래스터화 오차) 경우를 대비.
            }
            smoothed.push_back(path[farthest]);
            anchor = farthest;
        }

        path = std::move(smoothed);
    }
}
