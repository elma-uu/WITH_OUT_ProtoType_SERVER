#pragma once
#include "common.h"
#include "attack.h"

namespace Wop
{
    // Result of testing one ray against one player's approximate hitbox --
    // see TestRayAgainstPlayerCapsule.
    struct FHitResult
    {
        bool hit = false;
        float rayT = 0.0f;                    // distance along the ray to hitPosition
        ProtoType::Net::Vec3 hitPosition{};    // the actual point along the ray where it entered the capsule
        ProtoType::Net::HitBone hitBone = ProtoType::Net::HitBone::None;
    };

    // The server has no skeleton/mesh data for any player -- this models
    // the target as an upright capsule (a vertical segment swept by
    // kCapsuleRadius, i.e. exactly the shape of UE's own CapsuleComponent)
    // at their last-known tracked position, using BP_ProtoCharacter's
    // default ACharacter capsule dimensions (see the .cpp -- there's no way
    // for the server to know if a Blueprint ever overrides those in the
    // editor). This replaces the previous flat-sphere-around-a-point
    // approximation: it now actually accounts for the target's height and
    // facing, and populates HitBone by bucketing the hit height (and, for
    // the leg region, which side of the target's facing direction it
    // landed on) -- a single capsule has no separate arm geometry, so a
    // shot that would only graze an out-held arm simply passes outside the
    // capsule's radius and misses, same as it would against a torso-only
    // hitbox.
    //
    // rayOrigin/rayDirection: rayDirection MUST already be unit length.
    // targetPosition: the target's capsule CENTER -- GetActorLocation() for
    // an ACharacter (RootComponent is the capsule, so this is its
    // geometric center, not the feet).
    // targetYawDegrees: the target's facing (only Yaw matters -- see the
    // .cpp for why Pitch/Roll are ignored), for Left/Right leg bucketing.
    FHitResult TestRayAgainstPlayerCapsule(
        const ProtoType::Net::Vec3& rayOrigin, const ProtoType::Net::Vec3& rayDirection,
        const ProtoType::Net::Vec3& targetPosition, float targetYawDegrees,
        float maxRange);
}
