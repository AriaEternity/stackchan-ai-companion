#include <companion/motion_safety_plan.h>

#include <cassert>
#include <cstddef>

using namespace stackchan::companion;

namespace {

void expectTarget(const MotionTarget& target, int yaw, int pitch)
{
    assert(target.yaw == yaw);
    assert(target.pitch == pitch);
    assert(target.speed == kMotionSafetyTestSpeed);
    assert(target.holdMs == 0);
    assert(target.yaw >= kProjectYawLimitMinimum);
    assert(target.yaw <= kProjectYawLimitMaximum);
    assert(target.pitch >= kProjectPitchLimitMinimum);
    assert(target.pitch <= kProjectPitchLimitMaximum);
}

}  // namespace

int main()
{
    constexpr auto plan = makeMotionSafetyTestPlan();
    static_assert(plan.size() == kMotionSafetyTestTargetCount);

    expectTarget(plan[0], 0, 30);   // Fixed Home before any axis test.
    expectTarget(plan[1], 0, 70);   // Fixed conservative test center.
    expectTarget(plan[2], 60, 70);  // +Yaw.
    expectTarget(plan[3], 0, 70);
    expectTarget(plan[4], -60, 70);  // -Yaw.
    expectTarget(plan[5], 0, 70);
    expectTarget(plan[6], 0, 110);  // +Pitch.
    expectTarget(plan[7], 0, 70);
    expectTarget(plan[8], 0, 30);  // -Pitch reaches the safe lower bound.
    expectTarget(plan[9], 0, 30);  // Explicit final Home.

    const ProjectMotionSafety profile = makeConservativeMotionSafetyProfile(-1, 1);
    assert(profile.calibrated);
    assert(profile.yawCenter == 0);
    assert(profile.pitchCenter == 70);
    assert(profile.yawAmplitude == 60);
    assert(profile.pitchAmplitude == 40);
    assert(profile.yawDirection == -1);
    assert(profile.pitchDirection == 1);
    assert(profile.yawLimitMinimum == -60);
    assert(profile.yawLimitMaximum == 60);
    assert(profile.pitchLimitMinimum == 30);
    assert(profile.pitchLimitMaximum == 110);
    assert(profile.maximumSpeed == kMotionSafetyTestSpeed);
    return 0;
}
