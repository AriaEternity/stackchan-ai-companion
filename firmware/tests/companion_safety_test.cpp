/*
 * SPDX-License-Identifier: MIT
 */
#include <companion/safety_limits.h>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using stackchan::companion::canQueueCommand;
using stackchan::companion::clampServoSpeed;
using stackchan::companion::kCommandQueueCapacity;

void expectEqual(int actual, int expected, const char* label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void expectTrue(bool actual, bool expected, const char* label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void testSpeedClamp()
{
    expectEqual(clampServoSpeed(-1), 0, "speed below minimum");
    expectEqual(clampServoSpeed(0), 0, "speed minimum");
    expectEqual(clampServoSpeed(500), 500, "speed midpoint");
    expectEqual(clampServoSpeed(1000), 1000, "speed maximum");
    expectEqual(clampServoSpeed(1001), 1000, "speed above maximum");
    expectEqual(clampServoSpeed(std::numeric_limits<int>::min()), 0, "speed integer minimum");
    expectEqual(clampServoSpeed(std::numeric_limits<int>::max()), 1000, "speed integer maximum");
}

void testQueueCapacity()
{
    expectTrue(canQueueCommand(0), true, "empty queue accepts");
    expectTrue(canQueueCommand(kCommandQueueCapacity - 1), true, "last queue slot accepts");
    expectTrue(canQueueCommand(kCommandQueueCapacity), false, "full queue rejects");
    expectTrue(canQueueCommand(kCommandQueueCapacity + 1), false, "overfull queue rejects");
    expectTrue(canQueueCommand(std::numeric_limits<std::size_t>::max()), false, "size maximum rejects");
}

}  // namespace

int main()
{
    testSpeedClamp();
    testQueueCapacity();
    return 0;
}
