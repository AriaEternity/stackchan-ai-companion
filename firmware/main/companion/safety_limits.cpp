/*
 * SPDX-License-Identifier: MIT
 */
#include "safety_limits.h"

#include <algorithm>

namespace stackchan::companion {

int clampServoSpeed(int speed)
{
    return std::clamp(speed, kServoSpeedMinimum, kServoSpeedMaximum);
}

bool canQueueCommand(std::size_t queuedCommands)
{
    return queuedCommands < kCommandQueueCapacity;
}

}  // namespace stackchan::companion
