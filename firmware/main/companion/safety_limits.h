/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>

namespace stackchan::companion {

constexpr int kServoSpeedMinimum = 0;
constexpr int kServoSpeedMaximum = 1000;
constexpr std::size_t kCommandQueueCapacity = 4;

int clampServoSpeed(int speed);
bool canQueueCommand(std::size_t queuedCommands);

}  // namespace stackchan::companion
