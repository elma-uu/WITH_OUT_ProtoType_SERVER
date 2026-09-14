#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Wop
{
    // A single blocking obstacle, as a 2D (X/Y) axis-aligned box in Unreal
    // world units (cm). Height is deliberately ignored -- see this file's
    // header comment on LevelObstacles.
    struct FAabb2D
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
    };

    // A plain-text snapshot of a level's blocking geometry, loaded once at
    // startup, used to steer server-driven enemies (see EnemyAI.h) around
    // walls without any real NavMesh/pathfinding on the server -- this is
    // Registered I/O land, not Unreal Engine, so there IS no NavMesh here.
    // File format: one "minX minY maxX maxY" line per obstacle (see
    // WITH_OUT_ProtoType's ExportLevelObstaclesCommandlet, which produces
    // this file from the actual level in the editor). 2D only: height
    // doesn't matter for "can I walk in a straight line to my target",
    // which is all this is ever used for.
    class LevelObstacles
    {
    public:
        // Returns true and populates obstacles on success. Returns false
        // (leaving obstacles empty, i.e. "nothing blocks anything") if the
        // file doesn't exist or is empty -- callers should treat that as
        // "no obstacle data available yet", not an error; enemy AI just
        // falls back to unobstructed straight-line movement. Also builds
        // the A* occupancy grid (see FindPath) over the loaded obstacles'
        // own bounding box -- no NavMesh to build from, so this rasterizes
        // the same AABBs SegmentBlocked already uses.
        bool LoadFromFile(const std::string& path);

        // True if the line segment (fromX,fromY)->(toX,toY), inflated by
        // radius on every side, passes through any loaded obstacle. Not a
        // real raycast (no height, no partial-occlusion) -- a simple
        // segment-vs-AABB "slab" test per obstacle, same approximation
        // spirit as Session::ResolveAndBroadcastHit's hit detection.
        bool SegmentBlocked(float fromX, float fromY, float toX, float toY, float radius) const;

        // A* over a coarse occupancy grid rasterized from the same obstacle
        // AABBs SegmentBlocked uses (see BuildGrid) -- the server's actual
        // substitute for NavMesh/Detour (문제: "A* 알고리즘 적용해 줄 수
        // 있어?"). Returns true and fills outWaypoints (world-space X/Y,
        // string-pulled down to a handful of turning points -- see
        // SmoothPath -- not one entry per grid cell) with a path from
        // (fromX,fromY) to (toX,toY) if one exists within the grid.
        // Returns false (outWaypoints untouched) if the grid wasn't built
        // (no obstacle file loaded), either endpoint falls outside the
        // grid's bounds, or no path exists at all (start or goal cell
        // itself is blocked, or they're in disconnected pockets) -- the
        // caller (EnemyAI::Tick) falls back to its existing reactive
        // left/right steering in every one of those cases, same as it did
        // before this function existed.
        bool FindPath(float fromX, float fromY, float toX, float toY, float agentRadius,
                      std::vector<std::pair<float, float>>& outWaypoints) const;

        size_t Count() const { return obstacles_.size(); }

    private:
        // agentRadius is baked in at build time (not per-query) so the grid
        // only needs building once -- see kAgentRadius in the .cpp for the
        // value (matches EnemyAI.cpp's own kEnemyRadius, the same inflation
        // SegmentBlocked's callers already use for movement steering).
        void BuildGrid();

        // Greedy string-pulling: walks the raw cell-by-cell path and keeps
        // only the points where SegmentBlocked would stop a straight line
        // from the last KEPT point -- turns the "staircase" a raw grid path
        // always has into a handful of real turning points, so the enemy
        // actually walks straight lines between them instead of visibly
        // stair-stepping one cell at a time.
        void SmoothPath(std::vector<std::pair<float, float>>& path) const;

        std::vector<FAabb2D> obstacles_;

        std::vector<uint8_t> grid_; // 1 = blocked, 0 = free; row-major, gridW_ wide
        int gridW_ = 0;
        int gridH_ = 0;
        float gridMinX_ = 0.0f;
        float gridMinY_ = 0.0f;
        float cellSize_ = 150.0f;
    };
}
