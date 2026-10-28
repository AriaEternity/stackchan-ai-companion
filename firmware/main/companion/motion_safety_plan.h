/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "command_scheduler.h"

#include <array>
#include <cstddef>

namespace stackchan::companion {

constexpr int kMotionSafetyHomeYaw        = kProjectHomeYaw;
constexpr int kMotionSafetyHomePitch      = kProjectHomePitch;
constexpr int kMotionSafetyYawAmplitude   = 60;
constexpr int kMotionSafetyPitchAmplitude = 40;
constexpr int kMotionSafetyTestCenterYaw  = kMotionSafetyHomeYaw;
// Home is the pitch lower bound, so the test center must sit one amplitude
// above it to exercise both pitch directions without leaving the safe range.
constexpr int kMotionSafetyTestCenterPitch         = kMotionSafetyHomePitch + kMotionSafetyPitchAmplitude;
constexpr int kMotionSafetyTestSpeed               = 120;
constexpr std::uint32_t kMotionSafetyTestHoldMs    = 700;
constexpr std::size_t kMotionSafetyTestTargetCount = 10;

constexpr std::array<MotionTarget, kMotionSafetyTestTargetCount> makeMotionSafetyTestPlan()
{
    constexpr MotionTarget home{kMotionSafetyHomeYaw, kMotionSafetyHomePitch, kMotionSafetyTestSpeed, 0};
    constexpr MotionTarget center{kMotionSafetyTestCenterYaw, kMotionSafetyTestCenterPitch, kMotionSafetyTestSpeed, 0};
    return {{
        home,
        center,
        {kMotionSafetyTestCenterYaw + kMotionSafetyYawAmplitude, kMotionSafetyTestCenterPitch, kMotionSafetyTestSpeed,
         0},
        center,
        {kMotionSafetyTestCenterYaw - kMotionSafetyYawAmplitude, kMotionSafetyTestCenterPitch, kMotionSafetyTestSpeed,
         0},
        center,
        {kMotionSafetyTestCenterYaw, kMotionSafetyTestCenterPitch + kMotionSafetyPitchAmplitude, kMotionSafetyTestSpeed,
         0},
        center,
        {kMotionSafetyTestCenterYaw, kMotionSafetyTestCenterPitch - kMotionSafetyPitchAmplitude, kMotionSafetyTestSpeed,
         0},
        home,
    }};
}

constexpr ProjectMotionSafety makeConservativeMotionSafetyProfile(int yawDirection, int pitchDirection)
{
    ProjectMotionSafety candidate;
    candidate.calibrated        = true;
    candidate.yawCenter         = kMotionSafetyTestCenterYaw;
    candidate.pitchCenter       = kMotionSafetyTestCenterPitch;
    candidate.yawAmplitude      = kMotionSafetyYawAmplitude;
    candidate.pitchAmplitude    = kMotionSafetyPitchAmplitude;
    candidate.yawDirection      = yawDirection;
    candidate.pitchDirection    = pitchDirection;
    candidate.yawLimitMinimum   = candidate.yawCenter - candidate.yawAmplitude;
    candidate.yawLimitMaximum   = candidate.yawCenter + candidate.yawAmplitude;
    candidate.pitchLimitMinimum = candidate.pitchCenter - candidate.pitchAmplitude;
    candidate.pitchLimitMaximum = candidate.pitchCenter + candidate.pitchAmplitude;
    return candidate;
}

static_assert(kMotionSafetyHomeYaw >= kProjectYawLimitMinimum && kMotionSafetyHomeYaw <= kProjectYawLimitMaximum);
static_assert(kMotionSafetyHomePitch >= kProjectPitchLimitMinimum &&
              kMotionSafetyHomePitch <= kProjectPitchLimitMaximum);
static_assert(kMotionSafetyTestCenterPitch - kMotionSafetyPitchAmplitude >= kProjectPitchLimitMinimum);
static_assert(kMotionSafetyTestCenterPitch + kMotionSafetyPitchAmplitude <= kProjectPitchLimitMaximum);

}  // namespace stackchan::companion
