#include "LevelObstacles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace Wop
{
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
        return !obstacles_.empty();
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
}
