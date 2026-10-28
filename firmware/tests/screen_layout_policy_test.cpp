/*
 * SPDX-License-Identifier: MIT
 */
#include <companion/screen_layout_policy.h>

#include <cassert>
#include <cstdint>
#include <string>

using namespace stackchan::companion;

int main()
{
    assert(!shouldUseFullScreenText("neko is here."));
    assert(!shouldUseFullScreenText(std::string(kFullScreenTextThresholdBytes, 'a')));
    assert(shouldUseFullScreenText(std::string(kFullScreenTextThresholdBytes + 1, 'a')));
    assert(shouldUseFullScreenText("line one\nline two"));

    assert(!screenTextDeadlineReached(10'000, 20'000));
    assert(screenTextDeadlineReached(20'000, 20'000));
    assert(screenTextDeadlineReached(20'001, 20'000));
    assert(!screenTextDeadlineReached(20'000, 0));

    constexpr std::uint32_t nearWrap = 0xFFFFFFF0U;
    constexpr std::uint32_t wrappedDeadline = nearWrap + 32U;
    assert(!screenTextDeadlineReached(nearWrap, wrappedDeadline));
    assert(screenTextDeadlineReached(16U, wrappedDeadline));
    return 0;
}
