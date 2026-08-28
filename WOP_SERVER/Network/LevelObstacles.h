#pragma once
#include <string>
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
        // falls back to unobstructed straight-line movement.
        bool LoadFromFile(const std::string& path);

        // True if the line segment (fromX,fromY)->(toX,toY), inflated by
        // radius on every side, passes through any loaded obstacle. Not a
        // real raycast (no height, no partial-occlusion) -- a simple
        // segment-vs-AABB "slab" test per obstacle, same approximation
        // spirit as Session::ResolveAndBroadcastHit's hit detection.
        bool SegmentBlocked(float fromX, float fromY, float toX, float toY, float radius) const;

        size_t Count() const { return obstacles_.size(); }

    private:
        std::vector<FAabb2D> obstacles_;
    };
}
