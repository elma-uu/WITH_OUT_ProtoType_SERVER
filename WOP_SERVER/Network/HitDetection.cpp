#include "HitDetection.h"
#include <algorithm>
#include <cmath>

namespace Wop
{
    namespace
    {
        // Default UE ACharacter capsule -- neither AProtoCharacter's C++
        // base nor this server ever sees BP_ProtoCharacter's actual
        // CapsuleComponent size, so this is the best approximation
        // available (nothing currently reports capsule dimensions over the
        // wire). If a Blueprint ever resizes it in the editor, this drifts
        // out of sync with the real hitbox.
        constexpr float kCapsuleRadius = 34.0f;
        constexpr float kCapsuleHalfHeight = 88.0f;
        constexpr float kPi = 3.14159265358979323846f;

        struct FVec3f { float x, y, z; };

        FVec3f Sub(const FVec3f& a, const FVec3f& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        FVec3f Add(const FVec3f& a, const FVec3f& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        FVec3f Scale(const FVec3f& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        float Dot(const FVec3f& a, const FVec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    }

    FHitResult TestRayAgainstPlayerCapsule(
        const ProtoType::Net::Vec3& rayOrigin, const ProtoType::Net::Vec3& rayDirection,
        const ProtoType::Net::Vec3& targetPosition, float targetYawDegrees,
        float maxRange)
    {
        FHitResult result;

        const FVec3f O{ rayOrigin.x(), rayOrigin.y(), rayOrigin.z() };
        const FVec3f D{ rayDirection.x(), rayDirection.y(), rayDirection.z() }; // assumed unit
        const FVec3f Center{ targetPosition.x(), targetPosition.y(), targetPosition.z() };

        // The capsule's core segment: from the bottom hemisphere cap's
        // center (near the hips) to the top hemisphere cap's center (near
        // the neck). The capsule shape itself is this segment swept by
        // kCapsuleRadius -- same construction UE's own CapsuleComponent
        // collision uses.
        const float cylinderHalfLen = std::max(0.0f, kCapsuleHalfHeight - kCapsuleRadius);
        const FVec3f A = Add(Center, FVec3f{ 0.0f, 0.0f, -cylinderHalfLen });
        const FVec3f B = Add(Center, FVec3f{ 0.0f, 0.0f, cylinderHalfLen });
        const FVec3f AB = Sub(B, A);

        // Closest point between the ray's infinite LINE and the capsule
        // axis's infinite line (classic skew-line closest point). D is
        // unit length, so a = D.D = 1.
        const FVec3f r = Sub(O, A);
        const float a = Dot(D, D);
        const float e = Dot(AB, AB);
        const float bCoef = Dot(D, AB);
        const float c = Dot(D, r);
        const float f = Dot(AB, r);
        const float denom = a * e - bCoef * bCoef;

        float s = 0.0f;
        if (std::fabs(denom) > 1e-6f)
        {
            s = (a * f - bCoef * c) / denom;
        }
        // else: ray parallel to the capsule's vertical axis -- s=0 and the
        // correction pass below still finds a sane (if not perfectly
        // minimal) closest point.
        s = std::clamp(s, 0.0f, 1.0f);

        // Clamping s to the actual segment (rather than the infinite line)
        // means it may no longer be the true closest point on the LINE --
        // re-solve t as the ray's closest approach to this now-fixed point
        // Q(s). That's exact for "closest point on a line to a fixed
        // point", and simpler/more robust than the full 4-case
        // segment-vs-segment algorithm; the capsule (176cm) is tiny next to
        // maxRange (thousands of units), so one correction pass is
        // indistinguishable from the exact result.
        const FVec3f Q = Add(A, Scale(AB, s));
        const float t = Dot(D, Sub(Q, O));

        if (t < 0.0f || t > maxRange)
            return result; // behind the shooter, or beyond weapon range

        const FVec3f P = Add(O, Scale(D, t));
        const FVec3f toP = Sub(P, Q);
        const float distSq = Dot(toP, toP);

        if (distSq > kCapsuleRadius * kCapsuleRadius)
            return result; // ray passes outside the capsule

        result.hit = true;
        result.rayT = t;
        result.hitPosition = ProtoType::Net::Vec3(P.x, P.y, P.z);

        // Bucket the hit height into a body region: 0 = feet, 1 = top of
        // the head (top of the upper hemisphere cap).
        const float heightFrac = std::clamp(
            (P.z - (Center.z - kCapsuleHalfHeight)) / (2.0f * kCapsuleHalfHeight), 0.0f, 1.0f);

        if (heightFrac >= 0.85f)
        {
            result.hitBone = ProtoType::Net::HitBone::Head;
        }
        else if (heightFrac >= 0.55f)
        {
            result.hitBone = ProtoType::Net::HitBone::Chest;
        }
        else if (heightFrac >= 0.35f)
        {
            result.hitBone = ProtoType::Net::HitBone::Stomach;
        }
        else
        {
            // Below the waist -- only legs are modeled (see this
            // function's header comment on why arms aren't). Left/right is
            // whichever side of the target's own facing direction the hit
            // landed on. UE's yaw convention: Right = (-sin(yaw), cos(yaw), 0).
            const float yawRad = targetYawDegrees * kPi / 180.0f;
            const FVec3f Right{ -std::sin(yawRad), std::cos(yawRad), 0.0f };
            const float lateral = (P.x - Center.x) * Right.x + (P.y - Center.y) * Right.y;
            result.hitBone = (lateral >= 0.0f) ? ProtoType::Net::HitBone::RightLeg : ProtoType::Net::HitBone::LeftLeg;
        }

        return result;
    }
}
